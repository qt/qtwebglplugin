/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt WebGL module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qwebglwebsocketserver.h"

#include "qwebglcontext.h"
#include "qwebglfunctioncall.h"
#include "qwebglintegration.h"
#include "qwebglintegration_p.h"
#include "qwebglwindow.h"
#include "qwebglwindow_p.h"

#include <QtCore/private/qobject_p.h>
#include <QtCore/qcoreevent.h>
#include <QtCore/qdebug.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qwaitcondition.h>
#include <QtGui/qevent.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qpa/qwindowsysteminterface.h>
#include <QtWebSockets/qwebsocket.h>
#include <QtWebSockets/qwebsocketserver.h>

#include <cstring>

QT_BEGIN_NAMESPACE

static Q_LOGGING_CATEGORY(lc, "qt.qpa.webgl.websocketserver")

const QHash<QString, Qt::Key> keyMap {
    { "Alt", Qt::Key_Alt },
    { "ArrowDown", Qt::Key_Down },
    { "ArrowLeft", Qt::Key_Left },
    { "ArrowRight", Qt::Key_Right },
    { "ArrowUp", Qt::Key_Up },
    { "Backspace", Qt::Key_Backspace },
    { "Control", Qt::Key_Control },
    { "Delete", Qt::Key_Delete },
    { "End", Qt::Key_End },
    { "Enter", Qt::Key_Enter },
    { "F1", Qt::Key_F1 },
    { "F2", Qt::Key_F2 },
    { "F3", Qt::Key_F3 },
    { "F4", Qt::Key_F4 },
    { "F5", Qt::Key_F5 },
    { "F6", Qt::Key_F6 },
    { "F7", Qt::Key_F7 },
    { "F8", Qt::Key_F8 },
    { "F9", Qt::Key_F9 },
    { "F10", Qt::Key_F10 },
    { "F11", Qt::Key_F11 },
    { "F12", Qt::Key_F12 },
    { "Escape", Qt::Key_Escape },
    { "Home", Qt::Key_Home },
    { "Insert", Qt::Key_Insert },
    { "Meta", Qt::Key_Meta },
    { "PageDown", Qt::Key_PageDown },
    { "PageUp", Qt::Key_PageUp },
    { "Shift", Qt::Key_Shift },
    { "Space", Qt::Key_Space },
    { "AltGraph", Qt::Key_AltGr },
    { "Tab", Qt::Key_Tab },
    { "Unidentified", Qt::Key_F },
    { "OS", Qt::Key_Super_L }
};

inline QWebGLIntegration *webGLIntegration()
{
#ifdef QT_DEBUG
    auto nativeInterface = dynamic_cast<QWebGLIntegration *>(qGuiApp->platformNativeInterface());
    Q_ASSERT(nativeInterface);
#else
    auto nativeInterface = static_cast<QWebGLIntegration *>(qGuiApp->platformNativeInterface());
#endif // QT_DEBUG
    return nativeInterface;
}

class QWebGLWebSocketServerPrivate
{
public:
    QWebSocketServer *server = nullptr;
};

QWebGLWebSocketServer::QWebGLWebSocketServer(QObject *parent) :
    QObject(parent),
    d_ptr(new QWebGLWebSocketServerPrivate)
{}

QWebGLWebSocketServer::~QWebGLWebSocketServer()
{}

quint16 QWebGLWebSocketServer::port() const
{
    Q_D(const QWebGLWebSocketServer);
    return d->server->serverPort();
}

QMutex *QWebGLWebSocketServer::mutex()
{
    return &QWebGLIntegrationPrivate::instance()->waitMutex;
}

QWaitCondition *QWebGLWebSocketServer::waitCondition()
{
    return &QWebGLIntegrationPrivate::instance()->waitCondition;
}

QVariant QWebGLWebSocketServer::queryValue(int id)
{
    QMutexLocker locker(&QWebGLIntegrationPrivate::instance()->waitMutex);
    if (QWebGLIntegrationPrivate::instance()->receivedResponses.contains(id))
        return QWebGLIntegrationPrivate::instance()->receivedResponses.take(id);
    return QVariant();
}

void QWebGLWebSocketServer::create()
{
    Q_D(QWebGLWebSocketServer);
    d->server = new QWebSocketServer(QLatin1String("qtwebgl"), QWebSocketServer::NonSecureMode);
    bool ok = d->server->listen(QHostAddress::Any);
    if (ok)
        connect(d->server, &QWebSocketServer::newConnection, this,
                &QWebGLWebSocketServer::onNewConnection);

    QMutexLocker lock(&QWebGLIntegrationPrivate::instance()->waitMutex);
    QWebGLIntegrationPrivate::instance()->waitCondition.wakeAll();
}

void QWebGLWebSocketServer::sendMessage(QWebSocket *socket,
                                        MessageType type,
                                        const QVariantMap &values)
{
    if (!socket)
        return;
    QString typeString;
    switch (type) {
    case MessageType::Connect:
        typeString = QStringLiteral("connect");
        qCDebug(lc) << "Sending connect to " << socket << values;
        break;
    case MessageType::GlCommand: {
        const auto functionName = values["function"].toString().toUtf8();
        const auto parameters = values["parameters"].toList();
        const quint32 parameterCount = parameters.size();
        qCDebug(lc, "Sending gl_command %s to %p with %d parameters",
                qPrintable(functionName), socket, parameterCount);
        QByteArray data;
        {
            QDataStream stream(&data, QIODevice::WriteOnly);
            stream << functionName;
            if (values.contains("id")) {
                auto ok = false;
                stream << quint32(values["id"].toUInt(&ok));
                Q_ASSERT(ok);
            }
            stream << parameterCount;
            for (const auto &value : qAsConst(parameters)) {
                if (value.isNull()) {
                    stream << (quint8)'n';
                } else switch (value.type()) {
                case QVariant::Int:
                    stream << (quint8)'i' << value.toInt();
                    break;
                case QVariant::UInt:
                    stream << (quint8)'u' << value.toUInt();
                    break;
                case QVariant::Bool:
                    stream << (quint8)'b' << (quint8)value.toBool();
                    break;
                case QVariant::Double:
                    stream << (quint8)'d' << value.toDouble();
                    break;
                case QVariant::String:
                    stream << (quint8)'s' << value.toString().toUtf8();
                    break;
                case QVariant::ByteArray: {
                    const auto byteArray = value.toByteArray();
                    if (byteArray.isNull())
                        stream << (quint8)'n';
                    else
                        stream << (quint8)'x' << byteArray;
                    break;
                }
                default:
                    qCCritical(lc, "Unsupported type: %d", value.type());
                    break;
                }
            }
            stream << (quint32)0xbaadf00d;
        }
        const quint32 totalMessageSize = data.size();
        const quint32 maxMessageSize = 1024;
        for (quint32 i = 0; i <= data.size() / maxMessageSize; ++i) {
            const quint32 offset = i * maxMessageSize;
            const quint32 size = qMin(totalMessageSize - offset, maxMessageSize);
            const auto chunk = QByteArray::fromRawData(data.constData() + offset, size);
            socket->sendBinaryMessage(chunk);
        }
        return;
    }
    case MessageType::CreateCanvas:
        qCDebug(lc) << "Sending create_canvas to " << socket << values;
        typeString = QStringLiteral("create_canvas");
        break;
    case MessageType::DestroyCanvas:
        return; // TODO: In current implementation the canvas is not destroyed
        qCDebug(lc) << "Sending destroy_canvas to " << socket << values;
        typeString = QStringLiteral("destroy_canvas");
        break;
    case MessageType::OpenUrl:
        qCDebug(lc) << "Sending open_url to " << socket << values;
        typeString = QStringLiteral("open_url");
        break;
    case MessageType::ChangeTitle:
        qCDebug(lc) << "Sending change_title to " << socket << values;
        typeString = QStringLiteral("changle_title");
        break;
    }
    QJsonDocument document;
    auto commandObject = QJsonObject::fromVariantMap(values);
    commandObject["type"] = typeString;
    document.setObject(commandObject);
    auto data = document.toJson(QJsonDocument::Compact);
    socket->sendTextMessage(data);
}

bool QWebGLWebSocketServer::event(QEvent *event)
{
    int type = event->type();
    if (type == QWebGLFunctionCall::type()) {
        auto e = static_cast<QWebGLFunctionCall *>(event);
        QVariantMap values {
           { "function", e->functionName() },
           { "parameters", e->parameters() }
        };
        if (e->id() != -1)
            values.insert("id", e->id());
        auto integrationPrivate = QWebGLIntegrationPrivate::instance();
        auto clientData = integrationPrivate->findClientData(e->surface());
        if (clientData && clientData->socket) {
            sendMessage(clientData->socket, MessageType::GlCommand, values);
            if (e->isBlocking())
                integrationPrivate->pendingResponses.append(e->id());
            return true;
        }
        return false;
    }
    return QObject::event(event);
}

void QWebGLWebSocketServer::onNewConnection()
{
    Q_D(QWebGLWebSocketServer);
    QWebSocket *socket = d->server->nextPendingConnection();
    if (socket) {
        connect(socket, &QWebSocket::disconnected, this, &QWebGLWebSocketServer::onDisconnect);
        connect(socket, &QWebSocket::textMessageReceived, this,
                &QWebGLWebSocketServer::onTextMessageReceived);

        const QVariantMap values{
            {
                QStringLiteral("debug"),
#ifdef QT_DEBUG
                true
#else
                false
#endif
            },
            { QStringLiteral("loadingScreen"), qgetenv("QT_WEBGL_LOADINGSCREEN") },
            { "sysinfo",
                QVariantMap {
                    { QStringLiteral("buildAbi"), QSysInfo::buildAbi() },
                    { QStringLiteral("buildCpuArchitecture"), QSysInfo::buildCpuArchitecture() },
                    { QStringLiteral("currentCpuArchitecture"),
                      QSysInfo::currentCpuArchitecture() },
                    { QStringLiteral("kernelType"), QSysInfo::kernelType() },
                    { QStringLiteral("machineHostName"), QSysInfo::machineHostName() },
                    { QStringLiteral("prettyProductName"), QSysInfo::prettyProductName() },
                    { QStringLiteral("productType"), QSysInfo::productType() },
                    { QStringLiteral("productVersion"), QSysInfo::productVersion() }
                }
            }
        };

        sendMessage(socket, MessageType::Connect, values);
    }
}

void QWebGLWebSocketServer::onDisconnect()
{
    QWebSocket *socket = qobject_cast<QWebSocket *>(sender());
    Q_ASSERT(socket);
    QWebGLIntegrationPrivate::instance()->clientDisconnected(socket);
    socket->deleteLater();
}

void QWebGLWebSocketServer::onTextMessageReceived(const QString &message)
{
    const auto socket = qobject_cast<QWebSocket *>(sender());
    QWebGLIntegrationPrivate::instance()->onTextMessageReceived(socket, message);
}

QT_END_NAMESPACE
