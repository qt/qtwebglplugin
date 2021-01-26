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

#include "qwebglintegration.h"

#include <QtGui/qpa/qplatformintegrationplugin.h>

#include <cstring>

QT_BEGIN_NAMESPACE

class QWebGLIntegrationPlugin : public QPlatformIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "webgl.json")
public:
    QPlatformIntegration *create(const QString&, const QStringList&) override;
};

QPlatformIntegration* QWebGLIntegrationPlugin::create(const QString& system,
                                                      const QStringList& paramList)
{
    quint16 port = 8080;
    quint16 wssport = 0;

    if (!paramList.isEmpty()) {
        for (const QString &parameter : qAsConst(paramList)) {
            const QStringList parts = parameter.split('=');
            if (parts.first() == QStringLiteral("port")) {
                if (parts.size() != 2) {
                    qCCritical(lcWebGL, "Port parameter specified with no value");
                    return nullptr;
                }
                bool ok;
                port = parts.last().toUShort(&ok);
                if (!ok) {
                    qCCritical(lcWebGL, "Invalid port number");
                    return nullptr;
                }
            } else if (parts.first() == QStringLiteral("wsserverport")) {
                if (parts.size() != 2) {
                    qCCritical(lcWebGL, "Websocket server port specified with no value");
                    return nullptr;
                }
                bool ok;
                wssport = parts.last().toUShort(&ok);
                if (!ok) {
                    qCCritical(lcWebGL, "Invalid websocket port number");
                    return nullptr;
                }
            } else if (parts.first() == QStringLiteral("noloadingscreen"))
                qputenv("QT_WEBGL_LOADINGSCREEN", "0");
        }
    }
    if (!system.compare(QLatin1String("webgl"), Qt::CaseInsensitive))
        return new QWebGLIntegration(port, wssport);

    return nullptr;
}

QT_END_NAMESPACE

#include "qwebglmain.moc"
