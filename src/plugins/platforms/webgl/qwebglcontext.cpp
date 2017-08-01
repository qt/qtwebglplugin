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

#include "qwebglcontext.h"

#include "qwebglfunctioncall.h"
#include "qwebglintegration.h"
#include "qwebglintegration_p.h"
#include "qwebglwebsocketserver.h"
#include "qwebglwindow.h"
#include "qwebglwindow_p.h"

#include <QtCore/qhash.h>
#include <QtCore/qrect.h>
#include <QtCore/qset.h>
#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qimage.h>
#include <QtGui/qopenglcontext.h>
#include <QtGui/qsurface.h>
#include <QtWebSockets/qwebsocket.h>

#include <cstring>

QT_BEGIN_NAMESPACE

static Q_LOGGING_CATEGORY(lc, "qt.qpa.webgl.context")

struct PixelStorageModes
{
    PixelStorageModes() : unpackAlignment(4) { }
    int unpackAlignment;
};

struct ContextData {
    GLuint currentProgram = 0;
    GLuint boundArrayBuffer = 0;
    GLuint boundElementArrayBuffer = 0;
    GLuint boundTexture2D = 0;
    GLenum activeTextureUnit = GL_TEXTURE0;
    GLuint boundDrawFramebuffer = 0;
//    GLuint boundReadFramebuffer = 0;
    GLuint unpackAlignment = 4;
    struct VertexAttrib {
        VertexAttrib() : arrayBufferBinding(0), pointer(0), enabled(false) { }
        GLuint arrayBufferBinding;
        void *pointer;
        bool enabled;
        GLint size;
        GLenum type;
        bool normalized;
        GLsizei stride;
    };
    QHash<GLuint, VertexAttrib> vertexAttribPointers;
    QHash<GLuint, QImage> images;
    PixelStorageModes pixelStorage;
    QMap<GLenum, QVariant> cachedParameters;
    QSet<QByteArray> stringCache;
};

static QHash<int, ContextData> s_contextData;

QWebGLContext *currentContext()
{
    auto context = QOpenGLContext::currentContext();
    if (context)
        return static_cast<QWebGLContext *>(context->handle());
    return nullptr;
}

ContextData *currentContextData()
{
    auto context = currentContext();
    if (context)
        return &s_contextData[context->id()];
    return nullptr;
}

inline int imageSize(GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const PixelStorageModes &pixelStorage)
{
    Q_UNUSED(pixelStorage); // TODO: Support different pixelStorage formats

    static struct BppTabEntry {
        GLenum format;
        GLenum type;
        int bytesPerPixel;
    } bppTab[] = {
        { GL_RGBA, GL_UNSIGNED_BYTE, 4 },
        { GL_RGBA, GL_BYTE, 4 },
        { GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2 },
        { GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2 },
        { GL_RGBA, GL_FLOAT, 16 },
        { GL_RGB, GL_UNSIGNED_BYTE, 3 },
        { GL_RGB, GL_BYTE, 3 },
        { GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2 },
        { GL_RGB, GL_FLOAT, 12 },

        { GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2 },
        { GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 4 },
        { GL_DEPTH_COMPONENT, GL_FLOAT, 4 },

        { GL_RGBA, GL_UNSIGNED_BYTE, 4 },
        { GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2 },
        { GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2 },
        { GL_RGB, GL_UNSIGNED_BYTE, 3 },
        { GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2 },
        { GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 1 },
        { GL_LUMINANCE, GL_UNSIGNED_BYTE, 1 },
        { GL_ALPHA, GL_UNSIGNED_BYTE, 1 },

        { GL_BGRA_EXT, GL_UNSIGNED_BYTE, 4 },
        { GL_BGRA_EXT, GL_BYTE, 4 },
        { GL_BGRA_EXT, GL_UNSIGNED_SHORT_4_4_4_4, 2 },
        { GL_BGRA_EXT, GL_UNSIGNED_SHORT_5_5_5_1, 2 },
        { GL_BGRA_EXT, GL_FLOAT, 16 }
    };

    int bytesPerPixel = 0;
    for (size_t i = 0; i < sizeof(bppTab) / sizeof(BppTabEntry); ++i) {
        if (bppTab[i].format == format && bppTab[i].type == type) {
            bytesPerPixel = bppTab[i].bytesPerPixel;
            break;
        }
    }

    const int rowSize = width * bytesPerPixel;
    if (!bytesPerPixel)
        qCWarning(lc, "Unknown texture format %x - %x", format, type);

    return rowSize * height;
}

QByteArrayList strings;

static void lockMutex()
{
    QWebGLIntegrationPrivate::instance()->webSocketServer->mutex()->lock();
}

static void waitCondition(unsigned long time = ULONG_MAX)
{
    auto mutex = QWebGLIntegrationPrivate::instance()->webSocketServer->mutex();
    auto waitCondition = QWebGLIntegrationPrivate::instance()->webSocketServer->waitCondition();
    waitCondition->wait(mutex, time);
}

static void unlockMutex()
{
    auto mutex = QWebGLIntegrationPrivate::instance()->webSocketServer->mutex();
    mutex->unlock();
}

static QVariant queryValue(int id)
{
    return QWebGLContext::queryValue(id);
}

static int elementSize(GLenum type)
{
    switch (type) {
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
        return 2;
    case GL_FLOAT:
    case GL_FIXED:
    case GL_INT:
    case GL_UNSIGNED_INT:
        return 4;
    default:
        return 1;
    }
}

static int vertexSize(GLint elementsPerVertex, GLenum type)
{
    return elementSize(type) * elementsPerVertex;
}

static int bufferSize(GLsizei count, GLint elemsPerVertex, GLenum type, GLsizei stride)
{
    if (count == 0)
        return 0;

    int vsize = vertexSize(elemsPerVertex, type);

    if (stride == 0)
        stride = vsize;

    return vsize + (count - 1) * stride;
}

static void setVertexAttribs(QWebGLFunctionCall *event, GLsizei count)
{
    event->addInt(currentContextData()->vertexAttribPointers.count());
    QHashIterator<GLuint, ContextData::VertexAttrib> it(currentContextData()->vertexAttribPointers);
    while (it.hasNext()) {
        it.next();
        const ContextData::VertexAttrib &va(it.value());
        if (va.arrayBufferBinding == 0 && va.enabled) {
            int len = bufferSize(count, va.size, va.type, va.stride);
            event->addUInt(it.key());
            event->addInt(va.size);
            event->addInt(va.type);
            event->addInt(va.normalized);
            event->addInt(va.stride);
            // found an enabled vertex attribute that was specified with a client-side pointer
            event->addData(QByteArray((const char *)va.pointer, len));
        }
    }
}

namespace QWebGL {

static void glActiveTexture(GLenum texture)
{
    auto event = currentContext()->createEvent(QStringLiteral("activeTexture"));
    if (!event)
        return;
    event->addInt(texture);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    currentContextData()->activeTextureUnit = texture;
}

static void glAttachShader(GLuint program, GLuint shader)
{
    auto event = currentContext()->createEvent(QStringLiteral("attachShader"));
    if (!event)
        return;
    event->addUInt(program);
    event->addUInt(shader);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBindAttribLocation(GLuint program, GLuint index, const GLchar * name)
{
    auto event = currentContext()->createEvent(QStringLiteral("bindAttribLocation"));
    if (!event)
        return;
    event->addUInt(program);
    event->addUInt(index);
    event->addString(name);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBindBuffer(GLenum target, GLuint buffer)
{
    if (target == GL_ARRAY_BUFFER)
        currentContextData()->boundArrayBuffer = buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER)
        currentContextData()->boundElementArrayBuffer = buffer;

    auto event = currentContext()->createEvent(QStringLiteral("bindBuffer"));
    if (!event)
        return;
    event->addInt(target);
    event->addUInt(buffer);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    auto event = currentContext()->createEvent(QStringLiteral("bindFramebuffer"));
    if (!event)
        return;
    event->addInt(target);
    event->addUInt(framebuffer);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);

    if (target == GL_FRAMEBUFFER)
        currentContextData()->boundDrawFramebuffer = framebuffer;
}

static void glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
    auto event = currentContext()->createEvent(QStringLiteral("bindRenderbuffer"));
    if (!event)
        return;
    event->addInt(target);
    event->addUInt(renderbuffer);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBindTexture(GLenum target, GLuint texture)
{
    auto event = currentContext()->createEvent(QStringLiteral("bindTexture"));
    if (!event)
        return;
    event->addInt(target);
    event->addUInt(texture);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    if (target == GL_TEXTURE_2D)
        currentContextData()->boundTexture2D = texture;
}

static void glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    auto event = currentContext()->createEvent(QStringLiteral("blendColor"));
    if (!event)
        return;
    event->addFloat(red);
    event->addFloat(green);
    event->addFloat(blue);
    event->addFloat(alpha);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBlendEquation(GLenum mode)
{
    auto event = currentContext()->createEvent(QStringLiteral("blendEquation"));
    if (!event)
        return;
    event->addInt(mode);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha)
{
    auto event = currentContext()->createEvent(QStringLiteral("blendEquationSeparate"));
    if (!event)
        return;
    event->addInt(modeRGB);
    event->addInt(modeAlpha);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    auto event = currentContext()->createEvent(QStringLiteral("blendFunc"));
    if (!event)
        return;
    event->addInt(sfactor);
    event->addInt(dfactor);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha,
                                GLenum dfactorAlpha)
{
    auto event = currentContext()->createEvent(QStringLiteral("blendFuncSeparate"));
    if (!event)
        return;
    event->addInt(sfactorRGB);
    event->addInt(dfactorRGB);
    event->addInt(sfactorAlpha);
    event->addInt(dfactorAlpha);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBufferData(GLenum target, GLsizeiptr size, const void * data, GLenum usage)
{
    auto event = currentContext()->createEvent(QStringLiteral("bufferData"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(usage);
    event->addInt(size);
    if (data)
        event->addData(QByteArray((const char *)data, size));
    else
        event->addData(QByteArray());
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void * data)
{
    auto event = currentContext()->createEvent(QStringLiteral("bufferSubData"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(size);
    event->addInt(offset);
    event->addData(QByteArray((const char *)data, size));
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static GLenum glCheckFramebufferStatus(GLenum target)
{
    auto event = currentContext()->createEvent(QStringLiteral("checkFramebufferStatus"), true);
    if (!event) return -1;
    const auto id =  event->id();
    event->addInt(target);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    auto value = queryValue(id).toInt();
    return value;
}

static void glClear(GLbitfield mask)
{
    auto event = currentContext()->createEvent(QStringLiteral("clear"));
    if (!event)
        return;
    event->addInt(mask);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    auto event = currentContext()->createEvent(QStringLiteral("clearColor"));
    if (!event)
        return;
    event->addFloat(red);
    event->addFloat(green);
    event->addFloat(blue);
    event->addFloat(alpha);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glClearDepthf(GLfloat d)
{
    auto event = currentContext()->createEvent(QStringLiteral("clearDepthf"));
    if (!event)
        return;
    event->addFloat(d);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glClearStencil(GLint s)
{
    auto event = currentContext()->createEvent(QStringLiteral("clearStencil"));
    if (!event)
        return;
    event->addInt(s);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
    auto event = currentContext()->createEvent(QStringLiteral("colorMask"));
    if (!event)
        return;
    event->addInt(red);
    event->addInt(green);
    event->addInt(blue);
    event->addInt(alpha);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glCompileShader(GLuint shader)
{
    auto event = currentContext()->createEvent(QStringLiteral("compileShader"));
    if (!event)
        return;
    event->addUInt(shader);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                   GLsizei width, GLsizei height, GLint border,
                                   GLsizei imageSize, const void * data)
{
    auto event = currentContext()->createEvent(QStringLiteral("compressedTexImage2D"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(level);
    event->addInt(internalformat);
    event->addInt(width);
    event->addInt(height);
    event->addInt(border);
    event->addInt(imageSize);
    event->addData(QByteArray((const char *) data, imageSize));
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                      GLsizei width, GLsizei height, GLenum format,
                                      GLsizei imageSize, const void * data)
{
    auto event = currentContext()->createEvent(QStringLiteral("compressedTexSubImage2D"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(level);
    event->addInt(xoffset);
    event->addInt(yoffset);
    event->addInt(width);
    event->addInt(height);
    event->addInt(format);
    event->addInt(imageSize);
    event->addData(QByteArray((const char *) data, imageSize));
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x,
                             GLint y, GLsizei width, GLsizei height, GLint border)
{
    auto event = currentContext()->createEvent(QStringLiteral("copyTexImage2D"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(level);
    event->addInt(internalformat);
    event->addInt(x);
    event->addInt(y);
    event->addInt(width);
    event->addInt(height);
    event->addInt(border);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                GLint x, GLint y, GLsizei width, GLsizei height)
{
    auto event = currentContext()->createEvent(QStringLiteral("copyTexSubImage2D"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(level);
    event->addInt(xoffset);
    event->addInt(yoffset);
    event->addInt(x);
    event->addInt(y);
    event->addInt(width);
    event->addInt(height);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static GLuint glCreateProgram()
{
    auto event = currentContext()->createEvent(QStringLiteral("createProgram"), true);
    if (!event) return -1;
    const auto id =  event->id();
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toUInt();
    return value;
}

static GLuint glCreateShader(GLenum type)
{
    auto event = currentContext()->createEvent(QStringLiteral("createShader"), true);
    if (!event) return -1;
    const auto id =  event->id();
    event->addInt(type);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toUInt();
    return value;
}

static void glCullFace(GLenum mode)
{
    auto event = currentContext()->createEvent(QStringLiteral("cullFace"));
    if (!event)
        return;
    event->addInt(mode);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDeleteBuffers(GLsizei n, const GLuint * buffers)
{
    auto event = currentContext()->createEvent(QStringLiteral("deleteBuffers"));
    if (!event)
        return;
    event->addInt(n);
    for (int i = 0; i < n; ++i) {
        event->addUInt(buffers[i]);
        if (currentContextData()->boundArrayBuffer == buffers[i])
            currentContextData()->boundArrayBuffer = 0;
        if (currentContextData()->boundElementArrayBuffer == buffers[i])
            currentContextData()->boundElementArrayBuffer = 0;
    }
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDeleteFramebuffers(GLsizei n, const GLuint * framebuffers)
{
    auto event = currentContext()->createEvent(QStringLiteral("deleteFramebuffers"));
    if (!event)
        return;
    event->addInt(n);
    for (int i = 0; i < n; ++i)
        event->addUInt(framebuffers[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDeleteProgram(GLuint program)
{
    auto event = currentContext()->createEvent(QStringLiteral("deleteProgram"));
    if (!event)
        return;
    event->addUInt(program);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDeleteRenderbuffers(GLsizei n, const GLuint * renderbuffers)
{
    auto event = currentContext()->createEvent(QStringLiteral("deleteRenderbuffers"));
    if (!event)
        return;
    event->addInt(n);
    for (int i = 0; i < n; ++i)
        event->addUInt(renderbuffers[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDeleteShader(GLuint shader)
{
    auto event = currentContext()->createEvent(QStringLiteral("deleteShader"));
    if (!event)
        return;
    event->addUInt(shader);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDeleteTextures(GLsizei n, const GLuint * textures)
{
    auto event = currentContext()->createEvent(QStringLiteral("deleteTextures"));
    if (!event)
        return;
    event->addInt(n);
    for (int i = 0; i < n; ++i)
        event->addUInt(textures[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDepthFunc(GLenum func)
{
    auto event = currentContext()->createEvent(QStringLiteral("depthFunc"));
    if (!event)
        return;
    event->addInt(func);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDepthMask(GLboolean flag)
{
    auto event = currentContext()->createEvent(QStringLiteral("depthMask"));
    if (!event)
        return;
    event->addInt(flag);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDepthRangef(GLfloat n, GLfloat f)
{
    auto event = currentContext()->createEvent(QStringLiteral("depthRangef"));
    if (!event)
        return;
    event->addFloat(n);
    event->addFloat(f);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDetachShader(GLuint program, GLuint shader)
{
    auto event = currentContext()->createEvent(QStringLiteral("detachShader"));
    if (!event)
        return;
    event->addUInt(program);
    event->addUInt(shader);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDisableVertexAttribArray(GLuint index)
{
    auto event = currentContext()->createEvent(QStringLiteral("disableVertexAttribArray"));
    if (!event)
        return;
    currentContextData()->vertexAttribPointers[index].enabled = false;
    event->addUInt(index);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    auto event = currentContext()->createEvent(QStringLiteral("drawArrays"));
    if (!event)
        return;
    event->addInt(mode);
    event->addInt(first);
    event->addInt(count);
    // Some vertex attributes may be client-side, others may not. Therefore
    // client-side ones need to transfer the data starting from the base
    // pointer, not just from 'first'.
    setVertexAttribs(event, first + count);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void * indices)
{
    auto event = currentContext()->createEvent(QStringLiteral("drawElements"), false);
    if (!event)
        return;
    event->addInt(mode);
    event->addInt(count);
    event->addInt(type);
    setVertexAttribs(event, count);
    ContextData *d = currentContextData();
    if (d->boundElementArrayBuffer == 0) {
        event->addInt(0);
        QByteArray data((const char *) indices, count * elementSize(type));
        event->addData(data.data());
    } else {
        event->addInt(1);
        event->addUInt((quintptr) indices);
    }

    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glEnableVertexAttribArray(GLuint index)
{
    auto event = currentContext()->createEvent(QStringLiteral("enableVertexAttribArray"));
    if (!event)
        return;
    currentContextData()->vertexAttribPointers[index].enabled = true;
    event->addUInt(index);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glFinish()
{
    auto event = currentContext()->createEvent(QStringLiteral("finish"));
    if (!event)
        return;
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glFlush()
{
    auto event = currentContext()->createEvent(QStringLiteral("flush"));
    if (!event)
        return;
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                                      GLenum renderbuffertarget, GLuint renderbuffer)
{
    auto event = currentContext()->createEvent(QStringLiteral("framebufferRenderbuffer"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(attachment);
    event->addInt(renderbuffertarget);
    event->addUInt(renderbuffer);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                                       GLuint texture, GLint level)
{
    auto event = currentContext()->createEvent(QStringLiteral("framebufferTexture2D"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(attachment);
    event->addInt(textarget);
    event->addUInt(texture);
    event->addInt(level);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glFrontFace(GLenum mode)
{
    auto event = currentContext()->createEvent(QStringLiteral("frontFace"));
    if (!event)
        return;
    event->addInt(mode);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glGenBuffers(GLsizei n, GLuint* buffers)
{
    auto event = currentContext()->createEvent(QStringLiteral("genBuffers"), true);
    if (!event)
        return;
    const auto id =  event->id();
    event->addInt(n);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto values = queryValue(id).toList();
    if (values.size() != n) {
        qCWarning(lc, "Failed to create buffers");
        return;
    }
    for (int i = 0; i < n; ++i)
        buffers[i] = values.at(i).toUInt();
}

static void glGenFramebuffers(GLsizei n, GLuint* framebuffers)
{
    auto event = currentContext()->createEvent(QStringLiteral("genFramebuffers"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(n);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto values = queryValue(id).toList();
    if (values.size() != n) {
        qCWarning(lc, "Failed to create framebuffers");
        return;
    }
    for (int i = 0; i < n; ++i)
        framebuffers[i] = values.at(i).toUInt();
}

static void glGenRenderbuffers(GLsizei n, GLuint* renderbuffers)
{
    auto event = currentContext()->createEvent(QStringLiteral("genRenderbuffers"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(n);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto values = queryValue(id).toList();
    if (values.size() != n) {
        qCWarning(lc, "Failed to create render buffers");
        return;
    }
    for (int i = 0; i < n; ++i)
        renderbuffers[i] = values.at(i).toUInt();
}

static void glGenTextures(GLsizei n, GLuint* textures)
{
    auto event = currentContext()->createEvent(QStringLiteral("genTextures"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(n);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto variant = queryValue(id);
    const auto values = variant.toList();
    if (values.size() != n) {
        qCWarning(lc, "Failed to create textures");
        return;
    }
    for (int i = 0; i < n; ++i)
        textures[i] = values.at(i).toUInt();
}

static void glGenerateMipmap(GLenum target)
{
    auto event = currentContext()->createEvent(QStringLiteral("generateMipmap"));
    if (!event)
        return;
    event->addInt(target);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length,
                              GLint* size, GLenum* type, GLchar* name)
{
    auto event = currentContext()->createEvent(QStringLiteral("getActiveAttrib"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(program);
    event->addUInt(index);
    event->addInt(bufSize);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto values = queryValue(id).toMap();
    int rtype = values["rtype"].toInt();
    int rsize = values["rsize"].toInt();
    QByteArray rname = values["rname"].toByteArray();
    if (type)
        *type = rtype;
    if (size)
        *size = rsize;
    int len = qMax(0, qMin(bufSize - 1, rname.size()));
    if (length)
        *length = len;
    if (name) {
        memcpy(name, rname.constData(), len);
        name[len] = '\0';
    }
}

static void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length,
                               GLint* size, GLenum* type, GLchar* name)
{
    auto event = currentContext()->createEvent(QStringLiteral("getActiveUniform"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(program);
    event->addUInt(index);
    event->addInt(bufSize);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto values = queryValue(id).toMap();
    int rtype = values["rtype"].toInt();
    int rsize = values["rtype"].toInt();
    QByteArray rname = values["rtype"].toByteArray();
    if (type)
        *type = rtype;
    if (size)
        *size = rsize;
    int len = qMax(0, qMin(bufSize - 1, rname.size()));
    if (length)
        *length = len;
    if (name) {
        memcpy(name, rname.constData(), len);
        name[len] = '\0';
    }
}

static void glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei* count,
                                 GLuint* shaders)
{
    auto event = currentContext()->createEvent(QStringLiteral("getAttachedShaders"),
                                                       true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(program);
    event->addInt(maxCount);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto values = queryValue(id).toList();
    *count = values.size();
    for (int i = 0; i < values.size(); ++i)
        shaders[i] = values.at(i).toUInt();
}

static GLint glGetAttribLocation(GLuint program, const GLchar * name)
{
    auto event = currentContext()->createEvent(QStringLiteral("getAttribLocation"), true);
    if (!event) return -1;
    const auto id =  event->id();
    event->addUInt(program);
    event->addString(name);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toInt();
    return value;
}

static const GLubyte *glGetString(GLenum name)
{
    const auto it = currentContextData()->cachedParameters.find(name);
    if (it != currentContextData()->cachedParameters.end()) {
        auto &stringCache = currentContextData()->stringCache;
        Q_ASSERT(it->type() == QVariant::String);
        const auto string = it->toString().toLatin1();

        {
            auto it = stringCache.find(string), end = stringCache.end();
            if (it == end)
                it = stringCache.insert(string);
            return (const GLubyte *)(it->constData());
        }
    }

    auto event = currentContext()->createEvent(QStringLiteral("getString"), true);
    if (!event) return nullptr;
    const auto id =  event->id();
    event->addInt(name);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    auto value = queryValue(id).toByteArray();
    qCDebug(lc, "glGetString: %x: %s", name, qPrintable(value));
    strings.append(value);
    return (const GLubyte *)strings.last().constData();
}

static void glGetIntegerv(GLenum pname, GLint* data)
{
    if (pname == GL_MAX_TEXTURE_SIZE) {
        *data = 512;
        return;
    }

    const auto it = currentContextData()->cachedParameters.find(pname);
    if (it != currentContextData()->cachedParameters.end()) {
        QList<QVariant> values;
        switch (it->type()) {
        case QVariant::Map: values = it->toMap().values(); break;
        case QVariant::List: values = it->toList(); break;
        default: values = QVariantList{ *it };
        }
        for (const auto integer : qAsConst(values)) {
            bool ok;
            *data = integer.toInt(&ok);
            if (!ok)
                qCWarning(lc, "Failed to cast value");
            ++data;
        }
        return;
    }

    switch (pname) {
    case GL_CURRENT_PROGRAM:
        *data = currentContextData()->currentProgram;
        return;
    case GL_FRAMEBUFFER_BINDING:
        *data = currentContextData()->boundDrawFramebuffer;
        return;
    case GL_ARRAY_BUFFER_BINDING:
        *data = currentContextData()->boundArrayBuffer;
        return;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:
        *data = currentContextData()->boundElementArrayBuffer;
        return;
    case GL_ACTIVE_TEXTURE:
        *data = currentContextData()->activeTextureUnit;
        return;
    case GL_TEXTURE_BINDING_2D:
        *data = currentContextData()->boundTexture2D;
        return;
    default:;
    }

    auto event = currentContext()->createEvent(QStringLiteral("getIntegerv"), true);
    if (!event)
        return;
    const auto id =  event->id();
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toInt();
    qCDebug(lc, "glGetIntegerv: %x: %d", pname, value);
    *data = value;
}

static void glGetBooleanv(GLenum pname, GLboolean* data)
{
    const auto it = currentContextData()->cachedParameters.find(pname);
    if (it != currentContextData()->cachedParameters.end()) {
        Q_ASSERT(it->type() == QVariant::Bool);
        *data = it->toBool();
        return;
    }

    auto event = currentContext()->createEvent(QStringLiteral("getBooleanv"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toInt();
    qCDebug(lc, "glGetBooleanv: %x, %d", pname, value);
    *data = value;
}

static void glEnable(GLenum cap)
{
    auto event = currentContext()->createEvent(QStringLiteral("enable"));
    if (!event)
        return;
    event->addInt(cap);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);

    auto it = currentContextData()->cachedParameters.find(cap);
    if (it != currentContextData()->cachedParameters.end()) {
        Q_ASSERT(it->type() == QVariant::Bool);
        it->setValue(true);
    }
}

static void glDisable(GLenum cap)
{
    auto event = currentContext()->createEvent(QStringLiteral("disable"));
    if (!event)
        return;
    event->addInt(cap);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);

    auto it = currentContextData()->cachedParameters.find(cap);
    if (it != currentContextData()->cachedParameters.end()) {
        Q_ASSERT(it->type() == QVariant::Bool);
        it->setValue(false);
    }
}

static void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params)
{
    auto event = currentContext()->createEvent(QStringLiteral("getBufferParameteriv"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(target);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toInt();
    *params = value;
}

static GLenum glGetError()
{
    auto event = currentContext()->createEvent(QStringLiteral("getError"), true);
    if (!event) return GL_NO_ERROR;
    const auto id =  event->id();
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toInt();
    return value;
}

static void glGetFloatv(GLenum pname, GLfloat* data)
{
    auto event = currentContext()->createEvent(QStringLiteral("getParameter"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    const auto value = queryValue(id).toFloat(&ok);
    qCDebug(lc, "glGetFloatv: %x, %f", pname, value);
    if (!ok)
        qCCritical(lc, "Invalid value");
    else
        *data = value;
}

static void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment,
                                                  GLenum pname, GLint* params)
{
    auto event = currentContext()->createEvent(
                QStringLiteral("getFramebufferAttachmentParameteriv"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(target);
    event->addInt(attachment);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    const auto value = queryValue(id).toInt(&ok);
    if (!ok)
        qCCritical(lc, "Invalid value");
    else
        *params = value;
}

static void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length,
                                    GLchar* infoLog)
{
    auto event = currentContext()->createEvent(QStringLiteral("getProgramInfoLog"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(program);
    event->addInt(bufSize);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toString();
    *length = value.length();
    if (bufSize >= value.length())
        std::memcpy(infoLog, value.constData(), value.length());
}

static void glGetProgramiv(GLuint program, GLenum pname, GLint* params)
{
    auto event = currentContext()->createEvent(QStringLiteral("getProgramiv"), true);
    if (!event)
        return;
    const auto id =  event->id();
    event->addUInt(program);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    *params = queryValue(id).toInt();
}

static void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params)
{
    auto event = currentContext()->createEvent(QStringLiteral("getRenderbufferParameteriv"),
                                               true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(target);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    *params = queryValue(id).toInt();
}

static void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog)
{
    auto event = currentContext()->createEvent(QStringLiteral("getShaderInfoLog"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(shader);
    event->addInt(bufSize);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toString();
    *length = value.length();
    if (bufSize >= value.length())
        std::memcpy(infoLog, value.constData(), value.length());
}

static void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype, GLint* range,
                                       GLint* precision)
{
    auto event = currentContext()->createEvent(QStringLiteral("getShaderPrecisionFormat"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(shadertype);
    event->addInt(precisiontype);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    const auto value = queryValue(id).toMap();
    range[0] = value.value(QStringLiteral("rangeMin")).toInt(&ok);
    if (!ok)
        qCCritical(lc, "Invalid rangeMin value");
    range[1] = value.value(QStringLiteral("rangeMax")).toInt(&ok);
    if (!ok)
        qCCritical(lc, "Invalid rangeMax value");
    *precision = value.value(QStringLiteral("precision")).toInt(&ok);
    if (!ok)
        qCCritical(lc, "Invalid precision value");
}

static void glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source)
{
    auto event = currentContext()->createEvent(QStringLiteral("getShaderSource"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(shader);
    event->addInt(bufSize);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toString().toLatin1();
    *length = value.length();
    if (bufSize >= value.length())
        std::memcpy(source, value.constData(), value.length());
}

static void glGetShaderiv(GLuint shader, GLenum pname, GLint* params)
{
    if (pname == GL_INFO_LOG_LENGTH) {
        GLsizei bufSize = 0;
        glGetShaderInfoLog(shader, bufSize, &bufSize, nullptr);
        *params = bufSize;
        return;
    }
    if (pname == GL_SHADER_SOURCE_LENGTH) {
        GLsizei bufSize = 0;
        glGetShaderSource(shader, bufSize, &bufSize, nullptr);
        *params = bufSize;
        return;
    }
    auto event = currentContext()->createEvent(QStringLiteral("getShaderiv"), true);
    if (!event)
        return;
    const auto id =  event->id();
    event->addUInt(shader);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    *params = queryValue(id).toInt();
}

static void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat* params)
{
    auto event = currentContext()->createEvent(QStringLiteral("getTexParameterfv"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(target);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    *params = queryValue(id).toFloat(&ok);
    if (!ok)
        qCWarning(lc, "Failed to cast value");
}

static void glGetTexParameteriv(GLenum target, GLenum pname, GLint* params)
{
    auto event = currentContext()->createEvent(QStringLiteral("getTexParameteriv"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(target);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    *params = queryValue(id).toInt(&ok);
    if (!ok)
        qCWarning(lc, "Failed to cast value");
}

static GLint glGetUniformLocation(GLuint program, const GLchar * name)
{
    auto event = currentContext()->createEvent(QStringLiteral("getUniformLocation"), true);
    if (!event) return -1;
    const auto id =  event->id();
    event->addUInt(program);
    event->addString(name);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    const auto value = queryValue(id).toInt(&ok);
    if (!ok) {
        qCWarning(lc, "Failed to cast value");
        return -1;
    }
    return value;
}

static void glGetUniformfv(GLuint program, GLint location, GLfloat* params)
{
    auto event = currentContext()->createEvent(QStringLiteral("getUniformfv"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(program);
    event->addInt(location);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    const auto value = queryValue(id).toFloat(&ok);
    if (!ok)
        qCWarning(lc, "Failed to cast value");
    *params = value;
}

static void glGetUniformiv(GLuint program, GLint location, GLint* params)
{
    auto event = currentContext()->createEvent(QStringLiteral("getUniformiv"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(program);
    event->addInt(location);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    const auto value = queryValue(id).toInt(&ok);
    if (!ok)
        qCWarning(lc, "Failed to cast value");
    *params = value;
}

static void glGetVertexAttribPointerv(GLuint index, GLenum pname, void ** pointer)
{
    Q_UNUSED(pointer);
    qCCritical(lc, "Not supported");
    return;
    auto event = currentContext()->createEvent(QStringLiteral("getVertexAttribPointerv"));
    if (!event)
        return;
    event->addUInt(index);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat* params)
{
    auto event = currentContext()->createEvent(QStringLiteral("getVertexAttribfv"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(index);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    const auto value = queryValue(id).toFloat(&ok);
    if (!ok)
        qCWarning(lc, "Failed to cast value");
    *params = value;
}

static void glGetVertexAttribiv(GLuint index, GLenum pname, GLint* params)
{
    auto event = currentContext()->createEvent(QStringLiteral("getVertexAttribiv"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addUInt(index);
    event->addInt(pname);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    bool ok;
    const auto value = queryValue(id).toInt(&ok);
    if (!ok)
        qCWarning(lc, "Failed to cast value");
    *params = value;
}

static void glHint(GLenum target, GLenum mode)
{
    auto event = currentContext()->createEvent(QStringLiteral("hint"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(mode);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static GLboolean glIsBuffer(GLuint buffer)
{
    auto event = currentContext()->createEvent(QStringLiteral("isBuffer"), true);
    if (!event) return GL_FALSE;
    const auto id =  event->id();
    event->addUInt(buffer);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    auto value = queryValue(id).toInt();
    return value;
}

static GLboolean glIsEnabled(GLenum cap)
{
    auto event = currentContext()->createEvent(QStringLiteral("isEnabled"), true);
    if (!event) return GL_FALSE;
    const auto id =  event->id();
    event->addInt(cap);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    auto value = queryValue(id).toInt();
    return value;
}

static GLboolean glIsFramebuffer(GLuint framebuffer)
{
    auto event = currentContext()->createEvent(QStringLiteral("isFramebuffer"), true);
    if (!event) return GL_FALSE;
    const auto id =  event->id();
    event->addUInt(framebuffer);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toInt();
    return value;
}

static GLboolean glIsProgram(GLuint program)
{
    auto event = currentContext()->createEvent(QStringLiteral("isProgram"), true);
    if (!event) return GL_FALSE;
    const auto id =  event->id();
    event->addUInt(program);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toInt();
    return value;
}

static GLboolean glIsRenderbuffer(GLuint renderbuffer)
{
    auto event = currentContext()->createEvent(QStringLiteral("isRenderbuffer"), true);
    if (!event) return GL_FALSE;
    const auto id =  event->id();
    event->addUInt(renderbuffer);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toInt();
    return value;
}

static GLboolean glIsShader(GLuint shader)
{
    auto event = currentContext()->createEvent(QStringLiteral("isShader"), true);
    if (!event) return GL_FALSE;
    const auto id =  event->id();
    event->addUInt(shader);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = QWebGLIntegrationPrivate::instance()->webSocketServer->queryValue(id);
    return value.toInt();
}

static GLboolean glIsTexture(GLuint texture)
{
    auto event = currentContext()->createEvent(QStringLiteral("isTexture"), true);
    if (!event) return GL_FALSE;
    const auto id =  event->id();
    event->addUInt(texture);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toInt();
    return value;
}

static void glLineWidth(GLfloat width)
{
    auto event = currentContext()->createEvent(QStringLiteral("lineWidth"));
    if (!event)
        return;
    event->addFloat(width);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glLinkProgram(GLuint program)
{
    auto event = currentContext()->createEvent(QStringLiteral("linkProgram"));
    if (!event)
        return;
    event->addUInt(program);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glPixelStorei(GLenum pname, GLint param)
{
    switch (pname) {
    case GL_UNPACK_ALIGNMENT: currentContextData()->unpackAlignment = param; break;
    }

    auto event = currentContext()->createEvent(QStringLiteral("pixelStorei"));
    if (!event)
        return;
    event->addInt(pname);
    event->addInt(param);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glPolygonOffset(GLfloat factor, GLfloat units)
{
    auto event = currentContext()->createEvent(QStringLiteral("polygonOffset"));
    if (!event)
        return;
    event->addFloat(factor);
    event->addFloat(units);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                         GLenum type, void * pixels)
{
    auto event = currentContext()->createEvent(QStringLiteral("readPixels"), true);
    if (!event)
        return;
    const auto id = event->id();
    event->addInt(x);
    event->addInt(y);
    event->addInt(width);
    event->addInt(height);
    event->addInt(format);
    event->addInt(type);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    const auto value = queryValue(id).toByteArray();
    std::memcpy(pixels, value.constData(), value.size());
}

static void glReleaseShaderCompiler()
{
    auto event = currentContext()->createEvent(QStringLiteral("releaseShaderCompiler"));
    if (!event)
        return;
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width,
                                  GLsizei height)
{
    auto event = currentContext()->createEvent(QStringLiteral("renderbufferStorage"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(internalformat);
    event->addInt(width);
    event->addInt(height);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glSampleCoverage(GLfloat value, GLboolean invert)
{
    auto event = currentContext()->createEvent(QStringLiteral("sampleCoverage"));
    if (!event)
        return;
    event->addFloat(value);
    event->addInt(invert);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    auto event = currentContext()->createEvent(QStringLiteral("scissor"));
    if (!event)
        return;
    event->addInt(x);
    event->addInt(y);
    event->addInt(width);
    event->addInt(height);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glShaderBinary(GLsizei count, const GLuint * shaders, GLenum binaryformat,
                           const void * binary, GLsizei length)
{
    Q_UNUSED(count);
    Q_UNUSED(shaders);
    Q_UNUSED(binaryformat);
    Q_UNUSED(binary);
    Q_UNUSED(length);
    qFatal("WebGL does not allow precompiled shaders");
}

static void glShaderSource(GLuint shader, GLsizei count, const GLchar *const* string,
                           const GLint * length)
{
    auto event = currentContext()->createEvent(QStringLiteral("shaderSource"));
    if (!event)
        return;
    event->addUInt(shader);
    event->addInt(count);
    for (int i = 0; i < count; ++i) {
        if (!length)
            event->addString(QString::fromLatin1(string[i]));
        else
            event->addString(QString::fromLatin1(string[i], length[i]));
    }
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    auto event = currentContext()->createEvent(QStringLiteral("stencilFunc"));
    if (!event)
        return;
    event->addInt(func);
    event->addInt(ref);
    event->addUInt(mask);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask)
{
    auto event = currentContext()->createEvent(QStringLiteral("stencilFuncSeparate"));
    if (!event)
        return;
    event->addInt(face);
    event->addInt(func);
    event->addInt(ref);
    event->addUInt(mask);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glStencilMask(GLuint mask)
{
    auto event = currentContext()->createEvent(QStringLiteral("stencilMask"));
    if (!event)
        return;
    event->addUInt(mask);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glStencilMaskSeparate(GLenum face, GLuint mask)
{
    auto event = currentContext()->createEvent(QStringLiteral("stencilMaskSeparate"));
    if (!event)
        return;
    event->addInt(face);
    event->addUInt(mask);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
    auto event = currentContext()->createEvent(QStringLiteral("stencilOp"));
    if (!event)
        return;
    event->addInt(fail);
    event->addInt(zfail);
    event->addInt(zpass);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass)
{
    auto event = currentContext()->createEvent(QStringLiteral("stencilOpSeparate"));
    if (!event)
        return;
    event->addInt(face);
    event->addInt(sfail);
    event->addInt(dpfail);
    event->addInt(dppass);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                         GLsizei height, GLint border, GLenum format, GLenum type,
                         const void * pixels)
{
    auto event = currentContext()->createEvent(QStringLiteral("texImage2D"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(level);
    event->addInt(internalformat);
    event->addInt(width);
    event->addInt(height);
    event->addInt(border);
    event->addInt(format);
    event->addInt(type);

    if (pixels) {
        const int len = imageSize(width, height, format, type, currentContextData()->pixelStorage);
        event->addData(QByteArray((const char *)pixels, len));
    } else {
        event->addNull();
    }
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    auto event = currentContext()->createEvent(QStringLiteral("texParameterf"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(pname);
    event->addFloat(param);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glTexParameterfv(GLenum target, GLenum pname, const GLfloat * params)
{
    qFatal("glTexParameterfv not implemented");
    Q_UNUSED(target);
    Q_UNUSED(pname);
    Q_UNUSED(params);
}

static void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    auto event = currentContext()->createEvent(QStringLiteral("texParameteri"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(pname);
    event->addInt(param);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glTexParameteriv(GLenum target, GLenum pname, const GLint * params)
{
    qFatal("glTexParameteriv not implemented");
    Q_UNUSED(target);
    Q_UNUSED(pname);
    Q_UNUSED(params);
}

static void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                            GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const void * pixels)
{
    auto event = currentContext()->createEvent(QStringLiteral("texSubImage2D"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(level);
    event->addInt(xoffset);
    event->addInt(yoffset);
    event->addInt(width);
    event->addInt(height);
    event->addInt(format);
    event->addInt(type);
    if (pixels) {
        const int len = imageSize(width, height, format, type, currentContextData()->pixelStorage);
        event->addData(QByteArray((const char *)pixels, len));
    } else {
        event->addNull();
    }
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform1f(GLint location, GLfloat v0)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform1f"), false);
    if (!event)
        return;
    event->addInt(location);
    event->addFloat(v0);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform1fv(GLint location, GLsizei count, const GLfloat * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform1fv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    for (int i = 0; i < count; ++i)
        event->addFloat(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform1i(GLint location, GLint v0)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform1i"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(v0);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform1iv(GLint location, GLsizei count, const GLint * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform1iv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    for (int i = 0; i < count; ++i)
        event->addInt(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform2f(GLint location, GLfloat v0, GLfloat v1)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform2f"));
    if (!event)
        return;
    event->addInt(location);
    event->addFloat(v0);
    event->addFloat(v1);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform2fv(GLint location, GLsizei count, const GLfloat * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform2fv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    for (int i = 0; i < 2 * count; ++i)
        event->addFloat(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform2i(GLint location, GLint v0, GLint v1)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform2i"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(v0);
    event->addInt(v1);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform2iv(GLint location, GLsizei count, const GLint * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform2iv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    for (int i = 0; i < 2 * count; ++i)
        event->addInt(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform3f"));
    if (!event)
        return;
    event->addInt(location);
    event->addFloat(v0);
    event->addFloat(v1);
    event->addFloat(v2);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform3fv(GLint location, GLsizei count, const GLfloat * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform3fv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    for (int i = 0; i < 3 * count; ++i)
        event->addFloat(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform3i"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(v0);
    event->addInt(v1);
    event->addInt(v2);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform3iv(GLint location, GLsizei count, const GLint * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform3iv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    for (int i = 0; i < 3 * count; ++i)
        event->addInt(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform4f"));
    if (!event)
        return;
    event->addInt(location);
    event->addFloat(v0);
    event->addFloat(v1);
    event->addFloat(v2);
    event->addFloat(v3);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform4fv(GLint location, GLsizei count, const GLfloat * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform4fv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    for (int i = 0; i < 4 * count; ++i)
        event->addFloat(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform4i"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(v0);
    event->addInt(v1);
    event->addInt(v2);
    event->addInt(v3);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniform4iv(GLint location, GLsizei count, const GLint * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniform4iv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    for (int i = 0; i < 4 * count; ++i)
        event->addInt(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose,
                               const GLfloat * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniformMatrix2fv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    event->addInt(transpose);
    for (int i = 0; i < 4 * count; ++i)
        event->addFloat(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose,
                               const GLfloat * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniformMatrix3fv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    event->addInt(transpose);
    for (int i = 0; i < 9 * count; ++i)
        event->addFloat(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                               const GLfloat * value)
{
    auto event = currentContext()->createEvent(QStringLiteral("uniformMatrix4fv"));
    if (!event)
        return;
    event->addInt(location);
    event->addInt(count);
    event->addInt(transpose);
    for (int i = 0; i < 16 * count; ++i)
        event->addFloat(value[i]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glUseProgram(GLuint program)
{
    auto event = currentContext()->createEvent(QStringLiteral("useProgram"));
    if (!event)
        return;
    event->addUInt(program);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    currentContextData()->currentProgram = program;
}

static void glValidateProgram(GLuint program)
{
    auto event = currentContext()->createEvent(QStringLiteral("validateProgram"));
    if (!event)
        return;
    event->addUInt(program);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glVertexAttrib1f(GLuint index, GLfloat x)
{
    auto event = currentContext()->createEvent(QStringLiteral("vertexAttrib1f"));
    if (!event)
        return;
    event->addUInt(index);
    event->addFloat(x);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glVertexAttrib1fv(GLuint index, const GLfloat * v)
{
    auto event = currentContext()->createEvent(QStringLiteral("vertexAttrib1fv"));
    if (!event)
        return;
    event->addUInt(index);
    event->addFloat(v[0]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y)
{
    auto event = currentContext()->createEvent(QStringLiteral("vertexAttrib2f"));
    if (!event)
        return;
    event->addUInt(index);
    event->addFloat(x);
    event->addFloat(y);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glVertexAttrib2fv(GLuint index, const GLfloat * v)
{
    auto event = currentContext()->createEvent(QStringLiteral("vertexAttrib2fv"));
    if (!event)
        return;
    event->addUInt(index);
    event->addFloat(v[0]);
    event->addFloat(v[1]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z)
{
    auto event = currentContext()->createEvent(QStringLiteral("vertexAttrib3f"));
    if (!event)
        return;
    event->addUInt(index);
    event->addFloat(x);
    event->addFloat(y);
    event->addFloat(z);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glVertexAttrib3fv(GLuint index, const GLfloat * v)
{
    auto event = currentContext()->createEvent(QStringLiteral("vertexAttrib3fv"));
    if (!event)
        return;
    event->addUInt(index);
    event->addFloat(v[0]);
    event->addFloat(v[1]);
    event->addFloat(v[2]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    auto event = currentContext()->createEvent(QStringLiteral("vertexAttrib4f"));
    if (!event)
        return;
    event->addUInt(index);
    event->addFloat(x);
    event->addFloat(y);
    event->addFloat(z);
    event->addFloat(w);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glVertexAttrib4fv(GLuint index, const GLfloat * v)
{
    auto event = currentContext()->createEvent(QStringLiteral("vertexAttrib4fv"));
    if (!event)
        return;
    event->addUInt(index);
    event->addFloat(v[0]);
    event->addFloat(v[1]);
    event->addFloat(v[2]);
    event->addFloat(v[3]);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                  GLsizei stride, const void * pointer)
{
    ContextData *d = currentContextData();
    ContextData::VertexAttrib &va(d->vertexAttribPointers[index]);
    va.arrayBufferBinding = d->boundArrayBuffer;
    va.size = size;
    va.type = type;
    va.normalized = normalized;
    va.stride = stride;
    va.pointer = (void *) pointer;

    if (d->boundArrayBuffer) {
        auto event = currentContext()->createEvent(QStringLiteral("vertexAttribPointer"), false);
        if (!event)
            return;
        event->addUInt(index);
        event->addInt(size);
        event->addInt(type);
        event->addInt(normalized);
        event->addInt(stride);
        event->addUInt((quintptr) pointer);
        QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    }
}

static void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    auto event = currentContext()->createEvent(QStringLiteral("viewport"));
    if (!event)
        return;
    event->addInt(x);
    event->addInt(y);
    event->addInt(width);
    event->addInt(height);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);

    auto it = currentContextData()->cachedParameters.find(GL_VIEWPORT);
    if (it != currentContextData()->cachedParameters.end())
        it->setValue(QVariantList{ x, y, width, height });
}

static void glBlitFramebufferEXT(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                 GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                 GLbitfield mask, GLenum filter)
{
    auto event = currentContext()->createEvent(QStringLiteral("blitFramebufferEXT"));
    if (!event)
        return;
    event->addInt(srcX0);
    event->addInt(srcY0);
    event->addInt(srcX1);
    event->addInt(srcY1);
    event->addInt(dstX0);
    event->addInt(dstY0);
    event->addInt(dstX1);
    event->addInt(dstY1);
    event->addUInt(mask);
    event->addInt(filter);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glRenderbufferStorageMultisampleEXT(GLenum target, GLsizei samples,
                                                GLenum internalformat, GLsizei width,
                                                GLsizei height)
{
    auto event = currentContext()->createEvent(
                QStringLiteral("renderbufferStorageMultisampleEXT"));
    if (!event)
        return;
    event->addInt(target);
    event->addInt(samples);
    event->addInt(internalformat);
    event->addInt(width);
    event->addInt(height);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

static void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
    qFatal("glGetTexLevelParameteriv not supported");
    Q_UNUSED(target);
    Q_UNUSED(level);
    Q_UNUSED(pname);
    Q_UNUSED(params);
}

}

class QWebGLContextPrivate
{
public:
    int id = -1;
    static QAtomicInt nextId;
    static QSet<int> waitingIds;
    QPlatformSurface *currentSurface = nullptr;
    QSurfaceFormat surfaceFormat;
};

QAtomicInt QWebGLContextPrivate::nextId(1);
QSet<int> QWebGLContextPrivate::waitingIds;

QWebGLContext::QWebGLContext(const QSurfaceFormat &format) :
    d_ptr(new QWebGLContextPrivate)
{
    Q_D(QWebGLContext);
    d->id = d->nextId.fetchAndAddOrdered(1);

    qCDebug(lc, "Creating context %d", d->id);
    d->surfaceFormat = format;
    d->surfaceFormat.setRenderableType(QSurfaceFormat::OpenGLES);
}

QWebGLContext::~QWebGLContext()
{}

QSurfaceFormat QWebGLContext::format() const
{
    Q_D(const QWebGLContext);
    return d->surfaceFormat;
}

void QWebGLContext::swapBuffers(QPlatformSurface *surface)
{
    Q_UNUSED(surface);
    auto event = currentContext()->createEvent(QStringLiteral("swapBuffers"), true);
    if (!event)
        return;
    lockMutex();
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    waitCondition(1000);
    unlockMutex();
}

bool QWebGLContext::makeCurrent(QPlatformSurface *surface)
{
    Q_D(QWebGLContext);

    qCDebug(lc, "%p", surface);
    if (surface->surface()->surfaceClass() == QSurface::Window) {
        const auto window = static_cast<QWebGLWindow *>(surface);
        if (window->winId() == WId(-1))
            return false;
    }

    auto context = QOpenGLContext::currentContext();
    Q_ASSERT(context);
    auto handle = static_cast<QWebGLContext *>(context->handle());
    handle->d_func()->currentSurface = surface;
    auto event = currentContext()->createEvent(QStringLiteral("makeCurrent"));
    if (!event)
        return false;
    event->addInt(d->id);
    if (surface->surface()->surfaceClass() == QSurface::Window) {
        auto window = static_cast<QWebGLWindow *>(surface);
        if (s_contextData[handle->id()].cachedParameters.isEmpty()) {
            auto future = window->d_func()->defaults.get_future();
            std::future_status status = std::future_status::timeout;
            while (status == std::future_status::timeout) {
                if (!QWebGLIntegrationPrivate::instance()->findClientData(surface))
                    return false;
                status = future.wait_for(std::chrono::milliseconds(100));
            }
            s_contextData[handle->id()].cachedParameters  = future.get();
        }
        event->addInt(window->window()->width());
        event->addInt(window->window()->height());
        event->addInt(window->winId());
    } else if (surface->surface()->surfaceClass() == QSurface::Offscreen) {
        qCDebug(lc, "QWebGLContext::makeCurrent: QSurface::Offscreen not implemented");
    }
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
    return true;
}

void QWebGLContext::doneCurrent()
{
    auto event = currentContext()->createEvent(QStringLiteral("makeCurrent"));
    if (!event)
        return;
    event->addInt(0);
    event->addInt(0);
    event->addInt(0);
    event->addInt(0);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

bool QWebGLContext::isValid() const
{
    Q_D(const QWebGLContext);
    return d->id != -1;
}

QFunctionPointer QWebGLContext::getProcAddress(const char *procName)
{
    using namespace QWebGL;

    struct FuncTab {
        const char *name;
        QFunctionPointer func;
    } funcTab[] = {
#define QWEBGLCONTEXT_ADD_FUNCTION(NAME) { #NAME, (QFunctionPointer) QWebGL::NAME }
        QWEBGLCONTEXT_ADD_FUNCTION(glActiveTexture),
        QWEBGLCONTEXT_ADD_FUNCTION(glAttachShader),
        QWEBGLCONTEXT_ADD_FUNCTION(glBindAttribLocation),
        QWEBGLCONTEXT_ADD_FUNCTION(glBindBuffer),
        QWEBGLCONTEXT_ADD_FUNCTION(glBindFramebuffer),
        QWEBGLCONTEXT_ADD_FUNCTION(glBindRenderbuffer),
        QWEBGLCONTEXT_ADD_FUNCTION(glBindTexture),
        QWEBGLCONTEXT_ADD_FUNCTION(glBlendColor),
        QWEBGLCONTEXT_ADD_FUNCTION(glBlendEquation),
        QWEBGLCONTEXT_ADD_FUNCTION(glBlendEquationSeparate),
        QWEBGLCONTEXT_ADD_FUNCTION(glBlendFunc),
        QWEBGLCONTEXT_ADD_FUNCTION(glBlendFuncSeparate),
        QWEBGLCONTEXT_ADD_FUNCTION(glBufferData),
        QWEBGLCONTEXT_ADD_FUNCTION(glBufferSubData),
        QWEBGLCONTEXT_ADD_FUNCTION(glCheckFramebufferStatus),
        QWEBGLCONTEXT_ADD_FUNCTION(glClear),
        QWEBGLCONTEXT_ADD_FUNCTION(glClearColor),
        QWEBGLCONTEXT_ADD_FUNCTION(glClearDepthf),
        QWEBGLCONTEXT_ADD_FUNCTION(glClearStencil),
        QWEBGLCONTEXT_ADD_FUNCTION(glColorMask),
        QWEBGLCONTEXT_ADD_FUNCTION(glCompileShader),
        QWEBGLCONTEXT_ADD_FUNCTION(glCompressedTexImage2D),
        QWEBGLCONTEXT_ADD_FUNCTION(glCompressedTexSubImage2D),
        QWEBGLCONTEXT_ADD_FUNCTION(glCopyTexImage2D),
        QWEBGLCONTEXT_ADD_FUNCTION(glCopyTexSubImage2D),
        QWEBGLCONTEXT_ADD_FUNCTION(glCreateProgram),
        QWEBGLCONTEXT_ADD_FUNCTION(glCreateShader),
        QWEBGLCONTEXT_ADD_FUNCTION(glCullFace),
        QWEBGLCONTEXT_ADD_FUNCTION(glDeleteBuffers),
        QWEBGLCONTEXT_ADD_FUNCTION(glDeleteFramebuffers),
        QWEBGLCONTEXT_ADD_FUNCTION(glDeleteProgram),
        QWEBGLCONTEXT_ADD_FUNCTION(glDeleteRenderbuffers),
        QWEBGLCONTEXT_ADD_FUNCTION(glDeleteShader),
        QWEBGLCONTEXT_ADD_FUNCTION(glDeleteTextures),
        QWEBGLCONTEXT_ADD_FUNCTION(glDepthFunc),
        QWEBGLCONTEXT_ADD_FUNCTION(glDepthMask),
        QWEBGLCONTEXT_ADD_FUNCTION(glDepthRangef),
        QWEBGLCONTEXT_ADD_FUNCTION(glDetachShader),
        QWEBGLCONTEXT_ADD_FUNCTION(glDisable),
        QWEBGLCONTEXT_ADD_FUNCTION(glDisableVertexAttribArray),
        QWEBGLCONTEXT_ADD_FUNCTION(glDrawArrays),
        QWEBGLCONTEXT_ADD_FUNCTION(glDrawElements),
        QWEBGLCONTEXT_ADD_FUNCTION(glEnable),
        QWEBGLCONTEXT_ADD_FUNCTION(glEnableVertexAttribArray),
        QWEBGLCONTEXT_ADD_FUNCTION(glFinish),
        QWEBGLCONTEXT_ADD_FUNCTION(glFlush),
        QWEBGLCONTEXT_ADD_FUNCTION(glFramebufferRenderbuffer),
        QWEBGLCONTEXT_ADD_FUNCTION(glFramebufferTexture2D),
        QWEBGLCONTEXT_ADD_FUNCTION(glFrontFace),
        QWEBGLCONTEXT_ADD_FUNCTION(glGenBuffers),
        QWEBGLCONTEXT_ADD_FUNCTION(glGenFramebuffers),
        QWEBGLCONTEXT_ADD_FUNCTION(glGenRenderbuffers),
        QWEBGLCONTEXT_ADD_FUNCTION(glGenTextures),
        QWEBGLCONTEXT_ADD_FUNCTION(glGenerateMipmap),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetActiveAttrib),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetActiveUniform),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetAttachedShaders),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetAttribLocation),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetBooleanv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetBufferParameteriv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetError),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetFloatv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetFramebufferAttachmentParameteriv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetIntegerv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetProgramInfoLog),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetProgramiv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetRenderbufferParameteriv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetShaderInfoLog),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetShaderPrecisionFormat),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetShaderSource),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetShaderiv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetString),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetTexParameterfv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetTexParameteriv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetUniformLocation),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetUniformfv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetUniformiv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetVertexAttribPointerv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetVertexAttribfv),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetVertexAttribiv),
        QWEBGLCONTEXT_ADD_FUNCTION(glHint),
        QWEBGLCONTEXT_ADD_FUNCTION(glIsBuffer),
        QWEBGLCONTEXT_ADD_FUNCTION(glIsEnabled),
        QWEBGLCONTEXT_ADD_FUNCTION(glIsFramebuffer),
        QWEBGLCONTEXT_ADD_FUNCTION(glIsProgram),
        QWEBGLCONTEXT_ADD_FUNCTION(glIsRenderbuffer),
        QWEBGLCONTEXT_ADD_FUNCTION(glIsShader),
        QWEBGLCONTEXT_ADD_FUNCTION(glIsTexture),
        QWEBGLCONTEXT_ADD_FUNCTION(glLineWidth),
        QWEBGLCONTEXT_ADD_FUNCTION(glLinkProgram),
        QWEBGLCONTEXT_ADD_FUNCTION(glPixelStorei),
        QWEBGLCONTEXT_ADD_FUNCTION(glPolygonOffset),
        QWEBGLCONTEXT_ADD_FUNCTION(glReadPixels),
        QWEBGLCONTEXT_ADD_FUNCTION(glReleaseShaderCompiler),
        QWEBGLCONTEXT_ADD_FUNCTION(glRenderbufferStorage),
        QWEBGLCONTEXT_ADD_FUNCTION(glSampleCoverage),
        QWEBGLCONTEXT_ADD_FUNCTION(glScissor),
        QWEBGLCONTEXT_ADD_FUNCTION(glShaderBinary),
        QWEBGLCONTEXT_ADD_FUNCTION(glShaderSource),
        QWEBGLCONTEXT_ADD_FUNCTION(glStencilFunc),
        QWEBGLCONTEXT_ADD_FUNCTION(glStencilFuncSeparate),
        QWEBGLCONTEXT_ADD_FUNCTION(glStencilMask),
        QWEBGLCONTEXT_ADD_FUNCTION(glStencilMaskSeparate),
        QWEBGLCONTEXT_ADD_FUNCTION(glStencilOp),
        QWEBGLCONTEXT_ADD_FUNCTION(glStencilOpSeparate),
        QWEBGLCONTEXT_ADD_FUNCTION(glTexImage2D),
        QWEBGLCONTEXT_ADD_FUNCTION(glTexParameterf),
        QWEBGLCONTEXT_ADD_FUNCTION(glTexParameterfv),
        QWEBGLCONTEXT_ADD_FUNCTION(glTexParameteri),
        QWEBGLCONTEXT_ADD_FUNCTION(glTexParameteriv),
        QWEBGLCONTEXT_ADD_FUNCTION(glTexSubImage2D),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform1f),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform1fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform1i),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform1iv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform2f),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform2fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform2i),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform2iv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform3f),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform3fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform3i),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform3iv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform4f),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform4fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform4i),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniform4iv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniformMatrix2fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniformMatrix3fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUniformMatrix4fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glUseProgram),
        QWEBGLCONTEXT_ADD_FUNCTION(glValidateProgram),
        QWEBGLCONTEXT_ADD_FUNCTION(glVertexAttrib1f),
        QWEBGLCONTEXT_ADD_FUNCTION(glVertexAttrib1fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glVertexAttrib2f),
        QWEBGLCONTEXT_ADD_FUNCTION(glVertexAttrib2fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glVertexAttrib3f),
        QWEBGLCONTEXT_ADD_FUNCTION(glVertexAttrib3fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glVertexAttrib4f),
        QWEBGLCONTEXT_ADD_FUNCTION(glVertexAttrib4fv),
        QWEBGLCONTEXT_ADD_FUNCTION(glVertexAttribPointer),
        QWEBGLCONTEXT_ADD_FUNCTION(glViewport),
        QWEBGLCONTEXT_ADD_FUNCTION(glBlitFramebufferEXT),
        QWEBGLCONTEXT_ADD_FUNCTION(glRenderbufferStorageMultisampleEXT),
        QWEBGLCONTEXT_ADD_FUNCTION(glGetTexLevelParameteriv)
#undef QWEBGLCONTEXT_ADD_FUNCTION
    };


    auto size = sizeof(funcTab) / sizeof(FuncTab);
    for (auto i = 0u; i < size; ++i)
        if (strcmp(procName, funcTab[i].name) == 0)
            return funcTab[i].func;
    qCCritical(lc, "%s function not available", procName);
    return nullptr;
}

int QWebGLContext::id() const
{
    Q_D(const QWebGLContext);
    return d->id;
}

QPlatformSurface *QWebGLContext::currentSurface() const
{
    Q_D(const QWebGLContext);
    return d->currentSurface;
}

QWebGLFunctionCall *QWebGLContext::createEvent(const QString &functionName, bool wait)
{
    auto context = QOpenGLContext::currentContext();
    Q_ASSERT(context);
    const auto handle = static_cast<QWebGLContext *>(context->handle());
    auto integrationPrivate = QWebGLIntegrationPrivate::instance();
    const auto clientData = integrationPrivate->findClientData(handle->currentSurface());
    if (!clientData || !clientData->socket
            || clientData->socket->state() != QAbstractSocket::ConnectedState)
        return nullptr;
    const auto pointer = new QWebGLFunctionCall(functionName, handle->currentSurface(), wait);
    if (pointer) {
        if (wait)
            QWebGLContextPrivate::waitingIds.insert(pointer->id());
    } else {
        qCWarning(lc, "Cannot create the event, the client is disconnected");
    }

    return pointer;
}

QVariant QWebGLContext::queryValue(int id)
{
    if (!QWebGLContextPrivate::waitingIds.contains(id)) {
        qCWarning(lc, "Unexpected id (%d)", id);
        return QVariant();
    }

    static auto queryValue = [](int id)
    {
        lockMutex();
        waitCondition(10);
        unlockMutex();
        return QWebGLIntegrationPrivate::instance()->webSocketServer->queryValue(id);
    };

    const auto handle = static_cast<QWebGLContext *>(currentContext()->context()->handle());
    QVariant variant = queryValue(id);
    while (variant.isNull()) {
        auto integrationPrivate = QWebGLIntegrationPrivate::instance();
        const auto clientData = integrationPrivate->findClientData(handle->currentSurface());
        if (!clientData || !clientData->socket
                || clientData->socket->state() != QAbstractSocket::ConnectedState)
            return QVariant();
        variant = queryValue(id);
    }
    QWebGLContextPrivate::waitingIds.remove(id);
    return variant;
}

QT_END_NAMESPACE
