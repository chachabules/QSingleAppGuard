#ifndef GSINGLEAPPGUARD_H
#define GSINGLEAPPGUARD_H

#include "QSingleAppGuard_global.h"

#include <QDataStream>
#include <QDir>
#include <QRegularExpression>
#include <QTime>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>


class QSINGLEAPPGUARD_EXPORT QSingleAppGuard : public QObject
{
    Q_OBJECT
public:
    QSingleAppGuard(QObject *parent = 0, const QString &appId = QString());

    ~QSingleAppGuard();

    QString applicationId() const { return id_; }

    bool sendMessage(const QString & message, int timeout);

    bool isRunning() { return isClient(); }

private:
    void createSocket();

    void createLockFile();

    bool isClient();

private slots:
    void receiveConnection();

signals:
    void messageReceived(const QString &message);

private:
    QString id_;
    QString sock_name_;
    QLocalServer* server_;
    QLockFile* lock_file_;
    static const char* ack;
};

#endif // GSINGLEAPPGUARD_H
