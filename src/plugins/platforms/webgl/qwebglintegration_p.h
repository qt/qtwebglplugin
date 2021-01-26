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

#ifndef QWEBGLINTEGRATION_P_H
#define QWEBGLINTEGRATION_P_H

#include "qwebglscreen.h"
#include "qwebglhttpserver.h"
#include "qwebglplatformservices.h"
#include "qwebglwebsocketserver.h"

#include <QtCore/qmutex.h>
#include <QtCore/qwaitcondition.h>
#include <QtGui/qpa/qplatforminputcontextfactory_p.h>

#if defined(Q_OS_WIN)
#include <QtFontDatabaseSupport/private/qwindowsfontdatabase_p.h>
#include <QtEventDispatcherSupport/private/qwindowsguieventdispatcher_p.h>
#elif defined(Q_OS_MACOS)
#include <QtFontDatabaseSupport/private/qfontengine_coretext_p.h>
#include <QtFontDatabaseSupport/private/qcoretextfontdatabase_p.h>
#include <QtEventDispatcherSupport/private/qgenericunixeventdispatcher_p.h>
#else
#include <QtFontDatabaseSupport/private/qgenericunixfontdatabase_p.h>
#include <QtEventDispatcherSupport/private/qgenericunixeventdispatcher_p.h>
#endif // Q_OS_WIN

QT_BEGIN_NAMESPACE

class QWebSocket;
class QWebGLIntegration;

class QWebGLIntegrationPrivate
{
    Q_DECLARE_PUBLIC(QWebGLIntegration)
public:
    QWebGLIntegration *q_ptr = nullptr;

    struct ClientData
    {
        QVector<QWebGLWindow *> platformWindows;
        QWebSocket *socket;
        QWebGLScreen *platformScreen = nullptr;
    };

    mutable QPlatformInputContext *inputContext = nullptr;
    quint16 httpPort = 0;
    quint16 wssPort = 0;
#if defined(Q_OS_WIN)
    mutable QWindowsFontDatabase fontDatabase;
#elif defined(Q_OS_MACOS)
    mutable QCoreTextFontDatabaseEngineFactory<QCoreTextFontEngine> fontDatabase;
#else
    mutable QGenericUnixFontDatabase fontDatabase;
#endif
    mutable QWebGLPlatformServices services;
    QWebGLHttpServer *httpServer = nullptr;
    QWebGLWebSocketServer *webSocketServer = nullptr;
    QWebGLScreen *screen = nullptr;
    QThread *webSocketServerThread = nullptr;
    mutable struct {
        QList<ClientData> list;
        QMutex mutex;
    } clients;
    mutable QVector<QWindow *> windows;

    QMutex waitMutex;
    QWaitCondition waitCondition;
    QVector<int> pendingResponses;
    QHash<int, QVariant> receivedResponses;
    QTouchDevice *touchDevice = nullptr;

    ClientData *findClientData(const QWebSocket *socket);
    ClientData *findClientData(const QPlatformSurface *surface);
    QWebGLWindow *findWindow(const ClientData &clientData, WId winId);

    void clientConnected(QWebSocket *socket,
                           const int width,
                           const int height,
                           const double physicalWidth,
                           const double physicalHeight);
    void clientDisconnected(QWebSocket *socket);

    void connectNextClient();

    void sendMessage(QWebSocket *socket,
                     QWebGLWebSocketServer::MessageType type,
                     const QVariantMap &values) const;
    void onTextMessageReceived(QWebSocket *socket, const QString &message);
    void handleDefaultContextParameters(const ClientData &clientData, const QJsonObject &object);
    void handleGlResponse(const QJsonObject &object);
    void handleCanvasResize(const ClientData &clientData, const QJsonObject &object);
    void handleMouse(const ClientData &clientData, const QJsonObject &object);
    void handleWheel(const ClientData &clientData, const QJsonObject &object);
    void handleTouch(const ClientData &clientData, const QJsonObject &object);
    void handleKeyboard(const ClientData &clientData,
                        const QString &type,
                        const QJsonObject &object);

    Qt::KeyboardModifiers convertKeyboardModifiers(const QJsonObject &object);

    static QWebGLIntegrationPrivate *instance();
};

QT_END_NAMESPACE

#endif // QWEBGLINTEGRATION_P_H
