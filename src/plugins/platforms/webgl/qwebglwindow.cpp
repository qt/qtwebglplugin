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

#include "qwebglwindow.h"
#include "qwebglwindow_p.h"

#include "qwebglintegration_p.h"
#include "qwebglwebsocketserver.h"

#include <QtCore/qtextstream.h>
#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/private/qopenglcontext_p.h>
#include <QtGui/private/qwindow_p.h>
#include <QtGui/qpa/qwindowsysteminterface.h>
#include <QtGui/qpa/qplatformintegration.h>
#include <QtGui/qopenglcontext.h>
#include <QtGui/qoffscreensurface.h>

#include "qwebglwindow.h"

QT_BEGIN_NAMESPACE

static Q_LOGGING_CATEGORY(lc, "qt.qpa.webgl.window")

QAtomicInt QWebGLWindowPrivate::nextId(1);

QWebGLWindowPrivate::QWebGLWindowPrivate(QWebGLWindow *p) :
    q_ptr(p)
{}

QWebGLWindow::QWebGLWindow(QWindow *w) :
    QPlatformWindow(w),
    d_ptr(new QWebGLWindowPrivate(this))
{
}

QWebGLWindow::~QWebGLWindow()
{
    destroy();
}

void QWebGLWindow::create()
{
    Q_D(QWebGLWindow);
    if (d->flags.testFlag(QWebGLWindowPrivate::Created))
        return;

    d->id = QWebGLWindowPrivate::nextId.fetchAndAddAcquire(1);
    qCDebug(lc, "Window %d created", d->id);

    // Save the original surface type before changing to OpenGLSurface.
    d->raster = (window()->surfaceType() == QSurface::RasterSurface);
    if (d->raster) // change to OpenGL, but not for RasterGLSurface
        window()->setSurfaceType(QSurface::OpenGLSurface);

    if (window()->windowState() == Qt::WindowFullScreen) {
        QRect fullscreenRect(QPoint(), screen()->availableGeometry().size());
        QPlatformWindow::setGeometry(fullscreenRect);
        QWindowSystemInterface::handleGeometryChange(window(), fullscreenRect);
        return;
    }

    d->flags = QWebGLWindowPrivate::Created;

    if (window()->type() == Qt::Desktop)
        return;

    d->flags |= QWebGLWindowPrivate::HasNativeWindow;
    setGeometry(window()->geometry()); // will become fullscreen
    QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(0, 0), geometry().size()));

    if (d->raster) {
        QOpenGLContext *context = new QOpenGLContext(QGuiApplication::instance());
        context->setShareContext(qt_gl_global_share_context());
        context->setFormat(d->format);
        context->setScreen(window()->screen());
        if (Q_UNLIKELY(!context->create()))
            qFatal("QWebGL: Failed to create compositing context");
    }
}

void QWebGLWindow::destroy()
{
    Q_D(QWebGLWindow);
    qCDebug(lc, "Destroying %d", d->id);
    if (d->flags.testFlag(QWebGLWindowPrivate::HasNativeWindow)) {
        invalidateSurface();
    }

    qt_window_private(window())->updateRequestPending = false;

    d->flags = QWebGLWindowPrivate::Flags{};

    auto integrationPrivate = QWebGLIntegrationPrivate::instance();
    auto clientData = integrationPrivate->findClientData(surface()->surfaceHandle());
    if (clientData) {
        const QVariantMap values {
            { "winId", winId() }
        };
        if (clientData->socket)
            integrationPrivate->sendMessage(clientData->socket,
                                            QWebGLWebSocketServer::MessageType::DestroyCanvas,
                                            values);
        clientData->platformWindows.removeAll(this);
    }
}

void QWebGLWindow::raise()
{
    QWindow *wnd = window();
    if (wnd->type() != Qt::Desktop) {
        QWindowSystemInterface::handleExposeEvent(wnd, QRect(QPoint(0, 0), wnd->geometry().size()));
    }
}

QSurfaceFormat QWebGLWindow::format() const
{
    Q_D(const QWebGLWindow);
    return d->format;
}

QWebGLScreen *QWebGLWindow::screen() const
{
    return static_cast<QWebGLScreen *>(QPlatformWindow::screen());
}

void QWebGLWindow::setGeometry(const QRect &rect)
{
    QWindowSystemInterface::handleGeometryChange(window(), rect);
    QPlatformWindow::setGeometry(rect);
}

void QWebGLWindow::setDefaults(const QMap<GLenum, QVariant> &values)
{
    Q_D(QWebGLWindow);
    d->defaults.set_value(values);
}

WId QWebGLWindow::winId() const
{
    Q_D(const QWebGLWindow);
    return d->id;
}

QWebGLOffscreenSurface::QWebGLOffscreenSurface(QOffscreenSurface *offscreenSurface)
    : QPlatformOffscreenSurface(offscreenSurface)
{
}

QSurfaceFormat QWebGLOffscreenSurface::format() const
{
    return offscreenSurface()->format();
}

bool QWebGLOffscreenSurface::isValid() const
{
    return true;
}

QT_END_NAMESPACE
