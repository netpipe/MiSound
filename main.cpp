/*
Qt 5.12 C++ single-file app: MIDI network (server+client in one GUI)
- Uses RtMidi for local MIDI I/O
- Uses QTcpServer/QSslSocket for networking (SSL optional)
- Simple JSON-based protocol: announce, midi, ping, pong
- Visualizes connected peers and ping latency

Build notes (example qmake .pro parts):
QT += widgets network
CONFIG += c++11
# Add path to RtMidi include/lib as needed
LIBS += -lrtmidi

Compile: qmake && make

This is a compact example; production code should add better error handling.
*/

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QTimer>
#include <QDateTime>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QByteArray>
#include <QDataStream>
#include <QDebug>

#include <memory>
#include <map>
#include <atomic>

// RtMidi header - ensure RtMidi is installed and include path is known
#include "RtMidi.h"

// --- Helper: simple framed JSON protocol (4-byte length prefix) ---

static QByteArray packJson(const QJsonObject &obj) {
    QJsonDocument d(obj);
    QByteArray payload = d.toJson(QJsonDocument::Compact);
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << (quint32)payload.size();
    out.append(payload);
    return out;
}

static bool tryUnpackFromBuffer(QByteArray &buffer, QJsonObject &outObj) {
    if (buffer.size() < 4) return false;
    QDataStream ds(buffer);
    ds.setByteOrder(QDataStream::BigEndian);
    quint32 len = 0;
    ds >> len;
    if ((quint32)buffer.size() < 4 + len) return false;
    QByteArray payload = buffer.mid(4, len);
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // drop if invalid
        buffer.remove(0, 4 + len);
        return false;
    }
    outObj = doc.object();
    buffer.remove(0, 4 + len);
    return true;
}

// --- Peer representation ---
struct PeerInfo {
    QString id; // unique id
    QString name; // announced name (e.g., instrument)
    QHostAddress address;
    quint16 port = 0;
    qint64 lastSeenMs = 0;
    qint64 lastPingRtt = -1;
};

// --- MainWindow: combines server and client logic ---
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setupUi();
        setupMidi();
        setupNetwork();
        pingTimer = new QTimer(this);
        connect(pingTimer, &QTimer::timeout, this, &MainWindow::sendPings);
        pingTimer->start(3000); // every 3s

        uiUpdateTimer = new QTimer(this);
        connect(uiUpdateTimer, &QTimer::timeout, this, &MainWindow::refreshPeerList);
        uiUpdateTimer->start(1000);
    }
    ~MainWindow() {
        stopServer();
        stopClient();
        delete midiIn;
        delete midiOut;
    }

private slots:
    void toggleMode() {
        bool serverMode = (modeCombo->currentText() == "Server");
        addressEdit->setEnabled(!serverMode);
        startButton->setText(serverMode ? "Start Server" : "Connect");
    }
    void onStartClicked() {
        bool serverMode = (modeCombo->currentText() == "Server");
        if (serverMode) {
            startServer();
        } else {
            startClient();
        }
    }
    void onStopClicked() {
        stopServer();
        stopClient();
    }

    // Server slots
    void onNewConnection() {
        QTcpSocket *s = server->nextPendingConnection();
        if (!s) return;
        setupSocket(s);
        log("New incoming connection from " + s->peerAddress().toString());
    }

    // Client sockets
    void onConnected() {
        log("Connected to server");
        // announce
        sendAnnounce(clientSocket.get());
    }
    void onDisconnected() {
        log("Disconnected from server");
    }

    // Common when socket readable
    void onSocketReadyRead() {
        QTcpSocket *s = qobject_cast<QTcpSocket *>(sender());
        if (!s) return;
        QByteArray &buf = socketBuffers[s];
        buf.append(s->readAll());
        QJsonObject obj;
        while (tryUnpackFromBuffer(buf, obj)) {
            handleMessageFromSocket(s, obj);
        }
    }
    void onSslErrors(const QList<QSslError> &errors) {
        QSslSocket *ssl = qobject_cast<QSslSocket *>(sender());
        if (!ssl) return;
        QStringList parts;
        for (auto &e: errors) parts << e.errorString();
        log("SSL errors: " + parts.join(", "));
        // For demo: ignore SSL errors (NOT recommended in production)
        ssl->ignoreSslErrors();
    }

    // MIDI callbacks
    void handleLocalMidiInput(double deltatime, std::vector<unsigned char> *message, void *userData) {
        Q_UNUSED(deltatime);
        MainWindow *self = reinterpret_cast<MainWindow *>(userData);
        if (!self) return;
        if (!message || message->empty()) return;
        QByteArray bytes;
        for (unsigned char b : *message) bytes.append((char)b);
        self->sendMidiToNetwork(bytes);
        // also show locally
        emit self->midiReceivedLocally(bytes);
    }

    void onMidiReceivedLocally(const QByteArray &data) {
        // display
        QString s = QString("Local MIDI -> %1").arg(hexify(data));
        log(s);
    }

    // UI helpers
    void refreshPeerList() {
        peersList->clear();
        QMutexLocker locker(&peersMutex);
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto &kv : peers) {
            const PeerInfo &p = kv.second;
            QString label = QString("%1 (%2:%3) - %4 ms - %5s")
                    .arg(p.name)
                    .arg(p.address.toString())
                    .arg(p.port)
                    .arg(p.lastPingRtt >= 0 ? QString::number(p.lastPingRtt) : "--")
                    .arg((now - p.lastSeenMs) < 10000 ? "online" : "offline");
            peersList->addItem(label);
        }
    }

    void sendPings() {
        // ping all connected sockets
        QMutexLocker locker(&socketsMutex);
        for (QTcpSocket *s : activeSockets) {
            QJsonObject obj;
            obj["type"] = "ping";
            obj["ts"] = QDateTime::currentMSecsSinceEpoch();
            sendJsonOverSocket(s, obj);
        }
        // if client connected, also ping server
        if (clientSocket && clientSocket->state() == QAbstractSocket::ConnectedState) {
            QJsonObject obj;
            obj["type"] = "ping";
            obj["ts"] = QDateTime::currentMSecsSinceEpoch();
            sendJsonOverSocket(clientSocket.get(), obj);
        }
    }

private:
    // UI
    QWidget *central;
    QComboBox *modeCombo;
    QLineEdit *addressEdit;
    QLineEdit *portEdit;
    QCheckBox *sslCheck;
    QLineEdit *nameEdit;
    QPushButton *startButton;
    QPushButton *stopButton;
    QListWidget *peersList;
    QPlainTextEdit *logView;
    QComboBox *midiInCombo;
    QComboBox *midiOutCombo;

    // Network
    QTcpServer *server = nullptr;
    std::unique_ptr<QSslSocket> clientSocket; // used when client
    std::set<QTcpSocket *> activeSockets;
    QTimer *pingTimer = nullptr;
    QTimer *uiUpdateTimer = nullptr;
    QMutex socketsMutex;
    QMutex peersMutex;
    std::map<QString, PeerInfo> peers; // keyed by id
    std::map<QTcpSocket *, QByteArray> socketBuffers;

    // MIDI
    RtMidiIn *midiIn = nullptr;
    RtMidiOut *midiOut = nullptr;

    // Utility
    QString myId;

    void setupUi() {
        central = new QWidget(this);
        setCentralWidget(central);
        auto *mainLayout = new QVBoxLayout(central);

        // Top controls
        auto *topRow = new QHBoxLayout();
        modeCombo = new QComboBox();
        modeCombo->addItems({"Server", "Client"});
        addressEdit = new QLineEdit("127.0.0.1");
        portEdit = new QLineEdit("5000");
        sslCheck = new QCheckBox("Use SSL");
        nameEdit = new QLineEdit("Instrument-1");
        startButton = new QPushButton("Start Server");
        stopButton = new QPushButton("Stop");

        topRow->addWidget(new QLabel("Mode:")); topRow->addWidget(modeCombo);
        topRow->addWidget(new QLabel("Address:")); topRow->addWidget(addressEdit);
        topRow->addWidget(new QLabel("Port:")); topRow->addWidget(portEdit);
        topRow->addWidget(sslCheck);
        topRow->addWidget(new QLabel("Name:")); topRow->addWidget(nameEdit);
        topRow->addWidget(startButton);
        topRow->addWidget(stopButton);

        mainLayout->addLayout(topRow);

        // Middle: peers and MIDI
        auto *midRow = new QHBoxLayout();
        auto *peersBox = new QGroupBox("Peers / Instruments");
        auto *peersLayout = new QVBoxLayout(peersBox);
        peersList = new QListWidget();
        peersLayout->addWidget(peersList);
        midRow->addWidget(peersBox, 2);

        auto *midiBox = new QGroupBox("MIDI I/O");
        auto *midiLayout = new QVBoxLayout(midiBox);
        midiInCombo = new QComboBox();
        midiOutCombo = new QComboBox();
        midiLayout->addWidget(new QLabel("MIDI Input:")); midiLayout->addWidget(midiInCombo);
        midiLayout->addWidget(new QLabel("MIDI Output:")); midiLayout->addWidget(midiOutCombo);
        midRow->addWidget(midiBox, 1);

        mainLayout->addLayout(midRow);

        // Log
        logView = new QPlainTextEdit();
        logView->setReadOnly(true);
        mainLayout->addWidget(logView, 1);

        connect(modeCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(toggleMode()));
        connect(startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
        connect(stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);

        setWindowTitle("Qt MIDI Network (Server+Client)");
        resize(900, 600);
    }

    void setupMidi() {
        try {
            midiIn = new RtMidiIn();
            midiOut = new RtMidiOut();
        } catch (RtMidiError &err) {
            log(QString("RtMidi init error: %1").arg(QString::fromStdString(err.getMessage())));
            midiIn = nullptr;
            midiOut = nullptr;
            return;
        }
        // Populate device lists
        unsigned int nIn = midiIn->getPortCount();
        for (unsigned int i = 0; i < nIn; ++i) {
            midiInCombo->addItem(QString::fromStdString(midiIn->getPortName(i)));
        }
        unsigned int nOut = midiOut->getPortCount();
        for (unsigned int i = 0; i < nOut; ++i) {
            midiOutCombo->addItem(QString::fromStdString(midiOut->getPortName(i)));
        }
        if (nIn > 0) {
            midiIn->openPort(0);
            midiIn->ignoreTypes(false, false, false);
            // set callback
            midiIn->setCallback([](double deltatime, std::vector<unsigned char> *message, void *userData){
                MainWindow *self = reinterpret_cast<MainWindow *>(userData);
                if (self) self->handleLocalMidiInput(deltatime, message, userData);
            }, this);
        }
        if (nOut > 0) {
            midiOut->openPort(0);
        }
        connect(this, &MainWindow::midiReceivedLocally, this, &MainWindow::onMidiReceivedLocally);
    }

    void setupNetwork() {
        myId = QString::number(QDateTime::currentMSecsSinceEpoch()) + "-" + QString::number(qrand());
    }

    // --- Server control ---
    void startServer() {
        if (server) return;
        quint16 port = (quint16)portEdit->text().toUShort();
        server = new QTcpServer(this);
        connect(server, &QTcpServer::newConnection, this, &MainWindow::onNewConnection);
        if (!server->listen(QHostAddress::Any, port)) {
            log("Failed to start server: " + server->errorString());
            delete server; server = nullptr; return;
        }
        log(QString("Server listening on port %1").arg(port));
    }
    void stopServer() {
        if (!server) return;
        server->close();
        delete server; server = nullptr;
        QMutexLocker locker(&socketsMutex);
        for (QTcpSocket *s : activeSockets) {
            s->disconnectFromHost();
            s->deleteLater();
        }
        activeSockets.clear();
        socketBuffers.clear();
        log("Server stopped");
    }

    // --- Client control ---
    void startClient() {
        if (clientSocket && clientSocket->state() == QAbstractSocket::ConnectedState) return;
        QString addr = addressEdit->text();
        quint16 port = (quint16)portEdit->text().toUShort();
        bool useSsl = sslCheck->isChecked();
        QSslSocket *sock = new QSslSocket();
        clientSocket.reset(sock);
        connect(sock, &QSslSocket::connected, this, &MainWindow::onConnected);
        connect(sock, &QSslSocket::disconnected, this, &MainWindow::onDisconnected);
        connect(sock, &QSslSocket::readyRead, this, &MainWindow::onSocketReadyRead);
        connect(sock, QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors), this, &MainWindow::onSslErrors);
        if (useSsl) {
            // For demo: use default SSL configuration
            sock->connectToHostEncrypted(addr, port);
        } else {
            sock->connectToHost(addr, port);
        }
        log(QString("Connecting to %1:%2...").arg(addr).arg(port));
    }
    void stopClient() {
        if (clientSocket) {
            clientSocket->disconnectFromHost();
            clientSocket.reset();
            log("Client stopped");
        }
    }

    // --- Socket helpers ---
    void setupSocket(QTcpSocket *s) {
        if (!s) return;
        s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(s, &QTcpSocket::readyRead, this, &MainWindow::onSocketReadyRead);
        connect(s, &QTcpSocket::disconnected, [this, s]() {
            QMutexLocker locker(&socketsMutex);
            activeSockets.erase(s);
            socketBuffers.erase(s);
            s->deleteLater();
            log("Peer disconnected");
        });
        QMutexLocker locker(&socketsMutex);
        activeSockets.insert(s);
        socketBuffers[s] = QByteArray();
        // Immediately send announce to peer
        sendAnnounce(s);
    }

    void sendAnnounce(QTcpSocket *s) {
        QJsonObject obj;
        obj["type"] = "announce";
        obj["id"] = myId;
        obj["name"] = nameEdit->text();
        obj["port"] = portEdit->text().toInt();
        sendJsonOverSocket(s, obj);
    }
    void sendJsonOverSocket(QTcpSocket *s, const QJsonObject &obj) {
        if (!s) return;
        QByteArray out = packJson(obj);
        qint64 written = s->write(out);
        if (written <= 0) {
            log("Failed to write to socket: " + s->errorString());
        }
        s->flush();
    }

    void handleMessageFromSocket(QTcpSocket *s, const QJsonObject &obj) {
        QString type = obj.value("type").toString();
        if (type == "announce") {
            QString id = obj.value("id").toString();
            QString name = obj.value("name").toString();
            quint16 p = (quint16)obj.value("port").toInt();
            PeerInfo pi;
            pi.id = id; pi.name = name; pi.address = s->peerAddress(); pi.port = p; pi.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
            {
                QMutexLocker locker(&peersMutex);
                peers[id] = pi;
            }
            log(QString("Peer announced: %1 (%2)").arg(name).arg(s->peerAddress().toString()));
        } else if (type == "midi") {
            QString fromId = obj.value("from").toString();
            QByteArray midi = QByteArray::fromBase64(obj.value("data").toString().toUtf8());
            // play out local midiOut
            if (midiOut && midiOut->isPortOpen()) {
                std::vector<unsigned char> msg;
                for (char b : midi) msg.push_back((unsigned char)b);
                try { midiOut->sendMessage(&msg); }
                catch(...){}
            }
            QString sstr = QString("Remote MIDI from %1 -> %2").arg(fromId, hexify(midi));
            log(sstr);
        } else if (type == "ping") {
            // reply with pong carrying same ts
            qint64 ts = (qint64)obj.value("ts").toVariant().toLongLong();
            QJsonObject out;
            out["type"] = "pong";
            out["ts"] = ts;
            out["from"] = myId;
            sendJsonOverSocket(s, out);
        } else if (type == "pong") {
            qint64 ts = (qint64)obj.value("ts").toVariant().toLongLong();
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            qint64 rtt = now - ts;
            QString from = obj.value("from").toString();
            QMutexLocker locker(&peersMutex);
            if (peers.count(from)) {
                peers[from].lastPingRtt = rtt;
                peers[from].lastSeenMs = now;
            }
        }
    }

    void sendMidiToNetwork(const QByteArray &midi) {
        // package midi as base64 in JSON
        QJsonObject obj;
        obj["type"] = "midi";
        obj["from"] = myId;
        obj["data"] = QString::fromUtf8(midi.toBase64());
        // send to all connected peers (server role)
        {
            QMutexLocker locker(&socketsMutex);
            for (QTcpSocket *s : activeSockets) {
                sendJsonOverSocket(s, obj);
            }
        }
        // if client connected to server, forward
        if (clientSocket && clientSocket->state() == QAbstractSocket::ConnectedState) {
            sendJsonOverSocket(clientSocket.get(), obj);
        }
    }

    QString hexify(const QByteArray &d) {
        QStringList parts;
        for (unsigned char c : d.toHex()) {}
        for (int i=0;i<d.size();++i) parts << QString::asprintf("%02X", (unsigned char)d[i]);
        return parts.join(' ');
    }

    void log(const QString &s) {
        QString line = QDateTime::currentDateTime().toString(Qt::ISODate) + " - " + s;
        logView->appendPlainText(line);
        qDebug() << line;
    }

signals:
    void midiReceivedLocally(const QByteArray &data);
};

#include "moc_temp.moc"

int main(int argc, char **argv) {
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}
