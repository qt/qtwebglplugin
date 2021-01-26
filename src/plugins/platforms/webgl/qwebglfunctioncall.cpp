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

#include "qwebglfunctioncall.h"

#include <QtCore/qjsonobject.h>
#include <QtCore/qjsonvalue.h>
#include <QtCore/qstring.h>
#include <QtCore/qthread.h>
#include <QtGui/qpa/qplatformsurface.h>

QT_BEGIN_NAMESPACE

class QWebGLFunctionCallPrivate
{
public:
    QString functionName;
    QPlatformSurface *surface = nullptr;
    QVariantList parameters;
    bool wait = false;
    int id = -1;
    QThread *thread = nullptr;
    static QAtomicInt nextId;
    static int type;
};

QAtomicInt QWebGLFunctionCallPrivate::nextId(1);
int QWebGLFunctionCallPrivate::type(QEvent::registerEventType());

QWebGLFunctionCall::QWebGLFunctionCall(const QString &functionName,
                                       QPlatformSurface *surface,
                                       bool wait) :
    QEvent(type()),
    d_ptr(new QWebGLFunctionCallPrivate)
{
    Q_D(QWebGLFunctionCall);
    d->functionName = functionName;
    d->surface = surface;
    d->wait = wait;
    if (wait)
        d->id = QWebGLFunctionCallPrivate::nextId.fetchAndAddOrdered(1);
    d->thread = QThread::currentThread();
}

QWebGLFunctionCall::~QWebGLFunctionCall()
{}

QEvent::Type QWebGLFunctionCall::type()
{
    return Type(QWebGLFunctionCallPrivate::type);
}

int QWebGLFunctionCall::id() const
{
    Q_D(const QWebGLFunctionCall);
    return d->id;
}

QThread *QWebGLFunctionCall::thread() const
{
    Q_D(const QWebGLFunctionCall);
    return d->thread;
}

bool QWebGLFunctionCall::isBlocking() const
{
    Q_D(const QWebGLFunctionCall);
    return d->wait;
}

QPlatformSurface *QWebGLFunctionCall::surface() const
{
    Q_D(const QWebGLFunctionCall);
    return d->surface;
}

QString QWebGLFunctionCall::functionName() const
{
    Q_D(const QWebGLFunctionCall);
    return d->functionName;
}

void QWebGLFunctionCall::addString(const QString &value)
{
    Q_D(QWebGLFunctionCall);
    d->parameters.append(value);
}

void QWebGLFunctionCall::addInt(int value)
{
    Q_D(QWebGLFunctionCall);
    d->parameters.append(value);
}

void QWebGLFunctionCall::addUInt(uint value)
{
    Q_D(QWebGLFunctionCall);
    d->parameters.append(value);
}

void QWebGLFunctionCall::addFloat(float value)
{
    Q_D(QWebGLFunctionCall);
    d->parameters.append(static_cast<double>(value));
}

void QWebGLFunctionCall::addData(const QByteArray &data)
{
    Q_D(QWebGLFunctionCall);
    d->parameters.append(data);
}

void QWebGLFunctionCall::addList(const QVariantList &list)
{
    Q_D(QWebGLFunctionCall);
    d->parameters.append(QVariant::fromValue(list));
}

void QWebGLFunctionCall::addNull()
{
    Q_D(QWebGLFunctionCall);
    d->parameters.append(QVariant());
}

QVariantList QWebGLFunctionCall::parameters() const
{
    Q_D(const QWebGLFunctionCall);
    return d->parameters;
}

QT_END_NAMESPACE
