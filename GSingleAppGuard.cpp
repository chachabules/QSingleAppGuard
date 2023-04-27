#include <QCoreApplication>
#include "GSingleAppGuard.h"

#if defined(Q_OS_WIN)
#include <QLibrary>
#include <qt_windows.h>
typedef BOOL(WINAPI*PProcessIdToSessionId)(DWORD,DWORD*);
static PProcessIdToSessionId pProcessIdToSessionId = 0;
#endif
#if defined(Q_OS_UNIX)
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

const char* QSingleAppGuard::ack = "ack";
constexpr int lockfile_msec = 500;

QSingleAppGuard::QSingleAppGuard(QObject *parent, const QString &appId)
    : QObject(parent), id_(appId)
{
    QString prefix = id_;
    if (id_.isEmpty()) {
        id_ = QCoreApplication::applicationFilePath();
#if defined(Q_OS_WIN)
        id_ = id_.toLower();
#endif
        prefix = id_.section(QLatin1Char('/'), -1);
    }
    prefix.remove(QRegularExpression("[^a-zA-Z]"));
    prefix.truncate(6);

    QByteArray idc = id_.toUtf8();
    quint16 idNum = qChecksum(idc.constData(), idc.size());
    sock_name_ = QLatin1String("qtsingleapp-") + prefix
                 + QLatin1Char('-') + QString::number(idNum, 16);

#if defined(Q_OS_WIN)
    if (!pProcessIdToSessionId) {
        QLibrary lib("kernel32");
        pProcessIdToSessionId = (PProcessIdToSessionId)lib.resolve("ProcessIdToSessionId");
    }
    if (pProcessIdToSessionId) {
        DWORD sessionId = 0;
        pProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
        sock_name_ += QLatin1Char('-') + QString::number(sessionId, 16);
    }
#else
    sock_name_ += QLatin1Char('-') + QString::number(::getuid(), 16);
#endif

    createSocket();
    createLockFile();
}

QSingleAppGuard::~QSingleAppGuard()
{
    delete server_;
    server_ = nullptr;
    delete lock_file_;
    lock_file_ = nullptr;
}

bool QSingleAppGuard::sendMessage(const QString &message, int timeout = 5000)
{
    if (!isClient())
        return false;

    QLocalSocket socket;
    bool connOk = false;
    for(int i = 0; i < 2; i++) {
        // Try twice, in case the other instance is just starting up
        socket.connectToServer(sock_name_);
        connOk = socket.waitForConnected(timeout/2);
        if (connOk || i)
            break;
        int ms = 250;
#if defined(Q_OS_WIN)
        Sleep(DWORD(ms));
#else
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000 * 1000 };
        nanosleep(&ts, NULL);
#endif
    }
    if (!connOk)
        return false;

    QByteArray uMsg(message.toUtf8());
    QDataStream ds(&socket);
    ds.writeBytes(uMsg.constData(), uMsg.size());
    bool res = socket.waitForBytesWritten(timeout);
    if (res) {
        res &= socket.waitForReadyRead(timeout);   // wait for ack
        if (res)
            res &= (socket.read(qstrlen(ack)) == ack);
    }
    return res;
}

void QSingleAppGuard::createSocket()
{
    server_ = new QLocalServer(this);
}

void QSingleAppGuard::createLockFile()
{
    QString lockName = QDir(QDir::tempPath()).absolutePath()
                       + QLatin1Char('/') + sock_name_
                       + QLatin1String("-lockfile");
    lock_file_ = new QLockFile(lockName);
}

bool QSingleAppGuard::isClient()
{
    if (lock_file_->isLocked()) {
        return false;
    }

    if (!lock_file_->tryLock(lockfile_msec)) {
        return true;
    }

    bool res = server_->listen(sock_name_);
#if defined(Q_OS_UNIX) && (QT_VERSION >= QT_VERSION_CHECK(4,5,0))
    // ### Workaround
    if (!res && server_->serverError() == QAbstractSocket::AddressInUseError) {
        QFile::remove(QDir::cleanPath(QDir::tempPath())+QLatin1Char('/')+sock_name_);
        res = server_->listen(sock_name_);
    }
#endif
    if (!res)
        qWarning("QtSingleCoreApplication: listen on local socket failed, %s", qPrintable(server_->errorString()));
    connect(server_, &QLocalServer::newConnection, this, &QSingleAppGuard::receiveConnection);
    return false;
}

void QSingleAppGuard::receiveConnection()
{
    QLocalSocket* socket = server_->nextPendingConnection();
    if (!socket)
        return;

    while (true) {
        if (socket->state() == QLocalSocket::UnconnectedState) {
            qWarning("QtLocalPeer: Peer disconnected");
            delete socket;
            return;
        }
        if (socket->bytesAvailable() >= qint64(sizeof(quint32)))
            break;
        socket->waitForReadyRead();
    }

    QDataStream ds(socket);
    QByteArray uMsg;
    quint32 remaining;
    ds >> remaining;
    uMsg.resize(remaining);
    int got = 0;
    char* uMsgBuf = uMsg.data();
    do {
        got = ds.readRawData(uMsgBuf, remaining);
        remaining -= got;
        uMsgBuf += got;
    } while (remaining && got >= 0 && socket->waitForReadyRead(2000));
    if (got < 0) {
        qWarning("QtLocalPeer: Message reception failed %s", socket->errorString().toLatin1().constData());
        delete socket;
        return;
    }
    QString message(QString::fromUtf8(uMsg));
    socket->write(ack, qstrlen(ack));
    socket->waitForBytesWritten(1000);
    socket->waitForDisconnected(1000); // make sure client reads ack
    delete socket;
    emit messageReceived(message);
}
