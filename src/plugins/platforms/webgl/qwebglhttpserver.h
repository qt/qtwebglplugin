/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt WebGL module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:COMM$
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** $QT_END_LICENSE$
**
**
**
**
**
**
**
**
**
****************************************************************************/

#ifndef QWEBGLHTTPSERVER_H
#define QWEBGLHTTPSERVER_H

#include <QtCore/qobject.h>
#include <QtCore/qscopedpointer.h>
#include <QtNetwork/qhostaddress.h>

QT_BEGIN_NAMESPACE

class QIODevice;
class QTcpSocket;
class QString;
class QUrl;
class QWebGLWebSocketServer;
class QWebGLHttpServerPrivate;

class QWebGLHttpServer : public QObject
{
    Q_OBJECT

public:
    explicit QWebGLHttpServer(QWebGLWebSocketServer *webSocketServer, QObject *parent = nullptr);
    ~QWebGLHttpServer() override;

    bool listen(const QHostAddress &address = QHostAddress::Any, quint16 port = 0);
    bool isListening() const;
    quint16 serverPort() const;

    QIODevice *customRequestDevice(const QString &name);
    void setCustomRequestDevice(const QString &name, QIODevice *device);

    QString errorString() const;

private slots:
    void clientConnected();
    void clientDisconnected();
    void readData();
    void answerClient(QTcpSocket *socket, const QUrl &urls);

private:
    Q_DISABLE_COPY(QWebGLHttpServer)
    Q_DECLARE_PRIVATE(QWebGLHttpServer)
    QScopedPointer<QWebGLHttpServerPrivate> d_ptr;
};

QT_END_NAMESPACE

#endif // QWEBGLHTTPSERVER_H
