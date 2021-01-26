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

#ifndef QWEBGLWINDOW_P_H
#define QWEBGLWINDOW_P_H

#include <QtCore/qatomic.h>
#include <QtCore/qpointer.h>
#include <QtGui/qsurfaceformat.h>

#if (defined(Q_CC_MSVC) && _MSC_VER <= 1800)
// https://connect.microsoft.com/VisualStudio/feedback/details/811347/compiling-vc-12-0-with-has-exceptions-0-and-including-concrt-h-causes-a-compiler-error
_CRTIMP bool __cdecl __uncaught_exception();
#endif

#include <future>

QT_BEGIN_NAMESPACE

class QWebGLWindow;

class QWebGLWindowPrivate
{
public:
    QWebGLWindowPrivate(QWebGLWindow *p);

    bool raster = false;
    QSurfaceFormat format;

    enum Flag {
        Created = 0x01,
        HasNativeWindow = 0x02,
        IsFullScreen = 0x04
    };
    Q_DECLARE_FLAGS(Flags, Flag)
    Flags flags;

    std::promise<QMap<unsigned int, QVariant>> defaults;
    int id = -1;
    static QAtomicInt nextId;

private:
    Q_DECLARE_PUBLIC(QWebGLWindow)
    QWebGLWindow *q_ptr = nullptr;
};

QT_END_NAMESPACE

#endif // QWEBGLWINDOW_P_H
