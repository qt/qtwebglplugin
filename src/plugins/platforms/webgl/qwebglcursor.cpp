/****************************************************************************
**
** Copyright (C) 2011 by Etrnls
** etrnls@gmail.com
**
** This file is part of the Qt HTML platform plugin.
**
** Qt HTML platform plugin is free software: you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public License as
** published by the Free Software Foundation, either version 3 of the License,
** or (at your option) any later version.
**
** Qt HTML platform plugin is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with Qt HTML platform plugin. If not, see
** <http://www.gnu.org/licenses/>.
**
****************************************************************************/

#include "qwebglcursor.h"
#include "qwebglscreen.h"

<<<<<<< HEAD
#include <QtCore/QtDebug>

QWebGLCursor::QWebGLCursor(QWebGLScreen *screen/*, QObject *htmlService*/)
    : QPlatformCursor()/*,
      mHtmlService(htmlService)*/
=======
#include "qwebglintegration.h"
#include "qwebglintegration_p.h"

#include <QtGui/private/qguiapplication_p.h>
#include <QtCore/QtDebug>

QWebGLCursor::QWebGLCursor()
    : QPlatformCursor()
>>>>>>> 7b9234e90f74a46fd6af89d5ab39aec0d8537b3f
{
}

void QWebGLCursor::changeCursor(QCursor *windowCursor, QWindow *window)
{

<<<<<<< HEAD
    // Q_UNUSED(window);
    // QMetaObject::invokeMethod(mHtmlService, "changeCursor",
    //                           Q_ARG(int, static_cast<int>(windowCursor->shape())));
    

    //integrationPrivate->sendMessage(clientData.socket, QWebGLWebSocketServer::MessageType::ChangeCursor, static_cast<int>(windowCursor->shape());

    emit cursorChanged(windowCursor);
    
=======
    auto integrationPrivate = QWebGLIntegrationPrivate::instance();
    QSurface* surface = reinterpret_cast<QSurface*>(window);
    auto clientData = integrationPrivate->findClientData(surface->surfaceHandle());
    if (clientData) {
        const QVariantMap values {
            { "cursor", static_cast<int>(windowCursor->shape()) }
        };
        if (clientData->socket)
            integrationPrivate->sendMessage(clientData->socket,
                                            QWebGLWebSocketServer::MessageType::ChangeCursor,
                                            values);
    }
>>>>>>> 7b9234e90f74a46fd6af89d5ab39aec0d8537b3f
}

