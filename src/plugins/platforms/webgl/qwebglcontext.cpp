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
#include <QtCore/qpair.h>
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

template<class POINTER, class SIZE>
inline QWebGLFunctionCall *addHelper(QWebGLFunctionCall *e, const QPair<POINTER*, SIZE> &elements)
{
    if (e) {
        for (auto i = 0; i < elements.second; ++i)
            e->add(elements.first[i]);
    }
    return e;
}

template<class T>
inline QWebGLFunctionCall *addHelper(QWebGLFunctionCall *e, const T &value)
{
    if (e)
        e->add(value);
    return e;
}

template<class T, class... Ts>
inline QWebGLFunctionCall *addHelper(QWebGLFunctionCall *e, const T &value, const Ts&... rest)
{
    if (e) {
        e->add(value);
        addHelper(e, rest...);
    }
    return e;
}

template<class T>
static T queryValue(int id, const T &defaultValue = T())
{
    const auto variant = currentContext()->queryValue(id);
    if (variant.isNull())
        return defaultValue;
    if (!variant.canConvert<T>()) {
        qCWarning(lc, "Cannot convert %s to " QT_STRINGIFY(T), variant.typeName());
        return defaultValue;
    }
    return variant.value<T>();
}

struct GLFunction;
static QHash<QString, const GLFunction *> glFunctions;

struct GLFunction
{
    GLFunction(const QString &remoteName,
               const QString &localName,
               QFunctionPointer functionPointer)
        : remoteName(remoteName), localName(localName), functionPointer(functionPointer)
    {
        Q_ASSERT(!glFunctions.contains(localName));
        glFunctions.insert(localName, this);
    }

    GLFunction(const QString &name) : GLFunction(name, name, nullptr)
    {}

    const QString remoteName;
    const QString localName;
    const QFunctionPointer functionPointer;
};

template<const GLFunction *Function>
static QWebGLFunctionCall *createEvent(bool wait)
{
    auto context = QOpenGLContext::currentContext();
    Q_ASSERT(context);
    const auto handle = static_cast<QWebGLContext *>(context->handle());
    auto integrationPrivate = QWebGLIntegrationPrivate::instance();
    const auto clientData = integrationPrivate->findClientData(handle->currentSurface());
    if (!clientData || !clientData->socket
            || clientData->socket->state() != QAbstractSocket::ConnectedState)
        return nullptr;
    auto pointer = new QWebGLFunctionCall(Function->remoteName, handle->currentSurface(), wait);
    if (wait)
        QWebGLContextPrivate::waitingIds.insert(pointer->id());
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, pointer);
    return pointer;
}

template<const GLFunction *Function, class... Ts>
inline QWebGLFunctionCall *postEventImpl(bool wait, Ts&&... arguments)
{
    return addHelper(createEvent<Function>(wait), arguments...);
}

template<const GLFunction *Function>
inline QWebGLFunctionCall *postEventImpl(bool wait)
{
    return createEvent<Function>(wait);
}

template<const GLFunction *Function, class...Ts>
inline QWebGLFunctionCall *postEvent(Ts&&... arguments)
{
    return postEventImpl<Function>(false, arguments...);
}

template<const GLFunction *Function, class ReturnType, class...Ts>
static ReturnType postEventAndQuery(ReturnType defaultValue,
                                    Ts&&... arguments)
{
    const auto event = postEventImpl<Function>(true, arguments...);
    return event ? queryValue(event->id(), defaultValue) : defaultValue;
}

namespace QWebGL {

#define QWEBGL_FUNCTION(REMOTE_NAME, RET_TYPE, LOCAL_NAME, ...) \
    RET_TYPE LOCAL_NAME(__VA_ARGS__); \
    extern const GLFunction REMOTE_NAME { \
        #REMOTE_NAME, \
        #LOCAL_NAME, \
        (QFunctionPointer)LOCAL_NAME \
    }; \
    RET_TYPE LOCAL_NAME(__VA_ARGS__)

QWEBGL_FUNCTION(activeTexture, void, glActiveTexture, GLenum texture)
{
    postEvent<&activeTexture>(texture);
    currentContextData()->activeTextureUnit = texture;
}

QWEBGL_FUNCTION(attachShader, void, glAttachShader, GLuint program, GLuint shader)
{
    postEvent<&attachShader>(program, shader);
}

QWEBGL_FUNCTION(bindAttribLocation, void, glBindAttribLocation, GLuint program, GLuint index,
                 const GLchar *name)
{
    postEvent<&bindAttribLocation>(program, index, name);
}

QWEBGL_FUNCTION(bindBuffer, void, glBindBuffer, GLenum target, GLuint buffer)
{
    postEvent<&bindBuffer>(target, buffer);
    if (target == GL_ARRAY_BUFFER)
        currentContextData()->boundArrayBuffer = buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER)
        currentContextData()->boundElementArrayBuffer = buffer;
}

QWEBGL_FUNCTION(bindFramebuffer, void, glBindFramebuffer, GLenum target, GLuint framebuffer)
{
    postEvent<&bindFramebuffer>(target, framebuffer);
    if (target == GL_FRAMEBUFFER)
        currentContextData()->boundDrawFramebuffer = framebuffer;
}

QWEBGL_FUNCTION(bindRenderbuffer, void, glBindRenderbuffer, GLenum target, GLuint renderbuffer)
{
    postEvent<&bindRenderbuffer>(target, renderbuffer);
}

QWEBGL_FUNCTION(bindTexture, void, glBindTexture, GLenum target, GLuint texture)
{
    postEvent<&bindTexture>(target, texture);
    if (target == GL_TEXTURE_2D)
        currentContextData()->boundTexture2D = texture;
}

QWEBGL_FUNCTION(blendColor, void, glBlendColor, GLfloat red, GLfloat green, GLfloat blue,
                GLfloat alpha)
{
    postEvent<&blendColor>(red, green, blue, alpha);
}

QWEBGL_FUNCTION(blendEquation, void, glBlendEquation, GLenum mode)
{
    postEvent<&blendEquation>(mode);
}

QWEBGL_FUNCTION(blendEquationSeparate, void, glBlendEquationSeparate, GLenum modeRGB,
                GLenum modeAlpha)
{
    postEvent<&blendEquationSeparate>(modeRGB, modeAlpha);
}

QWEBGL_FUNCTION(blendFunc, void, glBlendFunc, GLenum sfactor, GLenum dfactor)
{
    postEvent<&blendFunc>(sfactor, dfactor);
}

QWEBGL_FUNCTION(blendFuncSeparate, void, glBlendFuncSeparate, GLenum sfactorRGB, GLenum dfactorRGB,
                GLenum sfactorAlpha, GLenum dfactorAlpha)
{
    postEvent<&blendFuncSeparate>(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha);
}

QWEBGL_FUNCTION(bufferData, void, glBufferData, GLenum target, GLsizeiptr size, const void *data,
                GLenum usage)
{
    postEvent<&bufferData>(target, usage, int(size), data ? QByteArray((const char *)data, size)
                                                          : QByteArray());
}

QWEBGL_FUNCTION(bufferSubData, void, glBufferSubData, GLenum target, GLintptr offset,
                GLsizeiptr size, const void *data)
{
    postEvent<&bufferSubData>(target, int(size), int(offset), QByteArray((const char *)data, size));
}

QWEBGL_FUNCTION(checkFramebufferStatus, GLenum, glCheckFramebufferStatus, GLenum target)
{
    return postEventAndQuery<&checkFramebufferStatus>(0u, target);
}

QWEBGL_FUNCTION(clear, void, glClear, GLbitfield mask)
{
    postEvent<&clear>(mask);
}

QWEBGL_FUNCTION(clearColor, void, glClearColor, GLfloat red, GLfloat green, GLfloat blue,
                GLfloat alpha)
{
    postEvent<&clearColor>(red, green, blue, alpha);
}

QWEBGL_FUNCTION(clearDepthf, void, glClearDepthf, GLfloat d)
{
    postEvent<&clearDepthf>(d);
}

QWEBGL_FUNCTION(clearStencil, void, glClearStencil, GLint s)
{
    postEvent<&clearStencil>(s);
}

QWEBGL_FUNCTION(colorMask, void, glColorMask, GLboolean red, GLboolean green, GLboolean blue,
                GLboolean alpha)
{
    postEvent<&colorMask>(red, green, blue, alpha);
}

QWEBGL_FUNCTION(compileShader, void, glCompileShader, GLuint shader)
{
    postEvent<&compileShader>(shader);
}

QWEBGL_FUNCTION(compressedTexImage2D, void, glCompressedTexImage2D, GLenum target, GLint level,
                GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                GLsizei imageSize, const void *data)
{
    postEvent<&compressedTexImage2D>(target, level, internalformat, width, height, border, imageSize,
                                    QByteArray((const char *) data, imageSize));
}

QWEBGL_FUNCTION(compressedTexSubImage2D, void, glCompressedTexSubImage2D, GLenum target,
                GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                GLenum format, GLsizei imageSize, const void *data)
{
    postEvent<&compressedTexSubImage2D>(target, level, xoffset, yoffset, width, height, format,
                                       imageSize, QByteArray((const char *)data, imageSize));
}

QWEBGL_FUNCTION(copyTexImage2D, void, glCopyTexImage2D, GLenum target, GLint level,
                GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height,
                GLint border)
{
    postEvent<&copyTexImage2D>(target, level, internalformat, x, y, width, height, border);
}

QWEBGL_FUNCTION(copyTexSubImage2D, void, glCopyTexSubImage2D, GLenum target, GLint level,
                GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    postEvent<&copyTexSubImage2D>(target, level, xoffset, yoffset, x, y, width, height);
}

QWEBGL_FUNCTION(createProgram, GLuint, glCreateProgram)
{
    return postEventAndQuery<&createProgram>(0u);
}

QWEBGL_FUNCTION(createShader, GLuint, glCreateShader, GLenum type)
{
    return postEventAndQuery<&createShader>(0u, type);
}

QWEBGL_FUNCTION(cullFace, void, glCullFace, GLenum mode)
{
    postEvent<&cullFace>(mode);
}

QWEBGL_FUNCTION(deleteBuffers, void, glDeleteBuffers, GLsizei n, const GLuint * buffers)
{
    postEvent<&deleteBuffers>(n, qMakePair(buffers, n));
    for (int i = 0; i < n; ++i) {
        if (currentContextData()->boundArrayBuffer == buffers[i])
            currentContextData()->boundArrayBuffer = 0;
        if (currentContextData()->boundElementArrayBuffer == buffers[i])
            currentContextData()->boundElementArrayBuffer = 0;
    }
}

QWEBGL_FUNCTION(deleteFramebuffers, void, glDeleteFramebuffers, GLsizei n,
                const GLuint *framebuffers)
{
    postEvent<&deleteFramebuffers>(n, qMakePair(framebuffers, n));
}

QWEBGL_FUNCTION(deleteProgram, void, glDeleteProgram, GLuint program)
{
    postEvent<&deleteProgram>(program);
}

QWEBGL_FUNCTION(deleteRenderbuffers, void, glDeleteRenderbuffers, GLsizei n,
                const GLuint *renderbuffers)
{
    postEvent<&deleteRenderbuffers>(n, qMakePair(renderbuffers, n));
}

QWEBGL_FUNCTION(deleteShader, void, glDeleteShader, GLuint shader)
{
    postEvent<&deleteShader>(shader);
}

QWEBGL_FUNCTION(deleteTextures, void, glDeleteTextures, GLsizei n, const GLuint * textures)
{
    postEvent<&deleteTextures>(n, qMakePair(textures, n));
}

QWEBGL_FUNCTION(depthFunc, void, glDepthFunc, GLenum func)
{
    postEvent<&depthFunc>(func);
}

QWEBGL_FUNCTION(depthMask, void, glDepthMask, GLboolean flag)
{
    postEvent<&depthMask>(flag);
}

QWEBGL_FUNCTION(depthRangef, void, glDepthRangef, GLfloat n, GLfloat f)
{
    postEvent<&depthRangef>(n, f);
}

QWEBGL_FUNCTION(detachShader, void, glDetachShader, GLuint program, GLuint shader)
{
    postEvent<&detachShader>(program, shader);
}

QWEBGL_FUNCTION(disableVertexAttribArray, void, glDisableVertexAttribArray, GLuint index)
{
    postEvent<&disableVertexAttribArray>(index);
    currentContextData()->vertexAttribPointers[index].enabled = false;
}

QWEBGL_FUNCTION(drawArrays, void, glDrawArrays, GLenum mode, GLint first, GLsizei count)
{
    auto event = currentContext()->createEvent(QStringLiteral("drawArrays"));
    if (!event)
        return;
    event->addParameters(mode, first, count);
    // Some vertex attributes may be client-side, others may not. Therefore
    // client-side ones need to transfer the data starting from the base
    // pointer, not just from 'first'.
    setVertexAttribs(event, first + count);
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

QWEBGL_FUNCTION(drawElements, void, glDrawElements, GLenum mode, GLsizei count, GLenum type,
                const void *indices)
{
    auto event = currentContext()->createEvent(QStringLiteral("drawElements"));
    if (!event)
        return;
    event->addParameters(mode, count, type);
    setVertexAttribs(event, count);
    ContextData *d = currentContextData();
    if (d->boundElementArrayBuffer == 0)
        event->addParameters(0, QByteArray((const char *) indices, count * elementSize(type)));
    else
        event->addParameters(1, uint(quintptr(indices)));
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

QWEBGL_FUNCTION(enableVertexAttribArray, void, glEnableVertexAttribArray, GLuint index)
{
    postEvent<&enableVertexAttribArray>(index);
    currentContextData()->vertexAttribPointers[index].enabled = true;
}

QWEBGL_FUNCTION(finish, void, glFinish)
{
    postEvent<&finish>();
}

QWEBGL_FUNCTION(flush, void, glFlush)
{
    postEvent<&flush>();
}

QWEBGL_FUNCTION(framebufferRenderbuffer, void, glFramebufferRenderbuffer, GLenum target,
                GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
{
    postEvent<&framebufferRenderbuffer>(target, attachment, renderbuffertarget, renderbuffer);
}

QWEBGL_FUNCTION(framebufferTexture2D, void, glFramebufferTexture2D, GLenum target,
                GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    postEvent<&framebufferTexture2D>(target, attachment, textarget, texture, level);
}

QWEBGL_FUNCTION(frontFace, void, glFrontFace, GLenum mode)
{
    postEvent<&frontFace>(mode);
}

QWEBGL_FUNCTION(genBuffers, void, glGenBuffers, GLsizei n, GLuint *buffers)
{
    const auto values = postEventAndQuery<&genBuffers>(QVariantList(), n);
    if (values.size() != n)
        qCWarning(lc, "Failed to create buffers");
    for (int i = 0; i < qMin(n, values.size()); ++i)
        buffers[i] = values.at(i).toUInt();
}

QWEBGL_FUNCTION(genFramebuffers, void, glGenFramebuffers, GLsizei n, GLuint *framebuffers)
{
    const auto values = postEventAndQuery<&genFramebuffers>(QVariantList(), n);
    if (values.size() != n)
        qCWarning(lc, "Failed to create framebuffers");
    for (int i = 0; i < qMin(n, values.size()); ++i)
        framebuffers[i] = values.at(i).toUInt();
}

QWEBGL_FUNCTION(genRenderbuffers, void, glGenRenderbuffers, GLsizei n, GLuint *renderbuffers)
{
    const auto values = postEventAndQuery<&genRenderbuffers>(QVariantList(), n);
    if (values.size() != n)
        qCWarning(lc, "Failed to create render buffers");
    for (int i = 0; i < qMin(n, values.size()); ++i)
        renderbuffers[i] = values.at(i).toUInt();
}

QWEBGL_FUNCTION(genTextures, void, glGenTextures, GLsizei n, GLuint *textures)
{
    const auto values = postEventAndQuery<&genTextures>(QVariantList(), n);
    if (values.size() != n)
        qCWarning(lc, "Failed to create textures");
    for (int i = 0; i < qMin(n, values.size()); ++i)
        textures[i] = values.at(i).toUInt();
}

QWEBGL_FUNCTION(generateMipmap, void, glGenerateMipmap, GLenum target)
{
    postEvent<&generateMipmap>(target);
}

QWEBGL_FUNCTION(getActiveAttrib, void, glGetActiveAttrib, GLuint program, GLuint index,
                GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name)
{
    const auto values = postEventAndQuery<&getActiveAttrib>(QVariantMap(), program, index, bufSize);
    if (values.isEmpty())
        return;
    const int rtype = values["rtype"].toInt();
    const int rsize = values["rsize"].toInt();
    const QByteArray rname = values["rname"].toByteArray();
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

QWEBGL_FUNCTION(getActiveUniform, void, glGetActiveUniform, GLuint program, GLuint index,
                GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name)
{
    const auto values = postEventAndQuery<&getActiveUniform>(QVariantMap(), program, index, bufSize);
    if (values.isEmpty())
        return;
    const int rtype = values["rtype"].toInt();
    const int rsize = values["rsize"].toInt();
    const QByteArray rname = values["rname"].toByteArray();
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

QWEBGL_FUNCTION(getAttachedShaders, void, glGetAttachedShaders, GLuint program, GLsizei maxCount,
                GLsizei *count, GLuint *shaders)
{
    const auto values = postEventAndQuery<&getAttachedShaders>(QVariantList(), program, maxCount);
    *count = values.size();
    for (int i = 0; i < values.size(); ++i)
        shaders[i] = values.at(i).toUInt();
}

QWEBGL_FUNCTION(getAttribLocation, GLint, glGetAttribLocation, GLuint program, const GLchar *name)
{
    return postEventAndQuery<&getAttribLocation>(-1, program, name);
}

QWEBGL_FUNCTION(getString, const GLubyte *, glGetString, GLenum name)
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
    const auto value = postEventAndQuery<&getString>(QByteArray(), name);
    strings.append(value);
    return (const GLubyte *)strings.last().constData();
}

QWEBGL_FUNCTION(getIntegerv, void, glGetIntegerv, GLenum pname, GLint *data)
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
    default:
        *data = postEventAndQuery<&getIntegerv>(0, pname);
    }
}

QWEBGL_FUNCTION(getBooleanv, void, glGetBooleanv, GLenum pname, GLboolean *data)
{
    const auto it = currentContextData()->cachedParameters.find(pname);
    if (it != currentContextData()->cachedParameters.end()) {
        Q_ASSERT(it->type() == QVariant::Bool);
        *data = it->toBool();
        return;
    }
    *data = postEventAndQuery<&getBooleanv>(GL_FALSE, pname);
}

QWEBGL_FUNCTION(enable, void, glEnable, GLenum cap)
{
    if (!postEvent<&enable>(cap))
        return;
    auto it = currentContextData()->cachedParameters.find(cap);
    if (it != currentContextData()->cachedParameters.end()) {
        Q_ASSERT(it->type() == QVariant::Bool);
        it->setValue(true);
    }
}

QWEBGL_FUNCTION(disable, void, glDisable, GLenum cap)
{
    if (!postEvent<&disable>(cap))
        return;
    auto it = currentContextData()->cachedParameters.find(cap);
    if (it != currentContextData()->cachedParameters.end()) {
        Q_ASSERT(it->type() == QVariant::Bool);
        it->setValue(false);
    }
}

QWEBGL_FUNCTION(getBufferParameteriv, void, glGetBufferParameteriv, GLenum target, GLenum pname,
                GLint *params)
{
    *params = postEventAndQuery<&getBufferParameteriv>(0, target, pname);
}

QWEBGL_FUNCTION(getError, GLenum, glGetError)
{
    return postEventAndQuery<&getError>(GL_NO_ERROR);
}

QWEBGL_FUNCTION(getParameter, void, glGetFloatv, GLenum pname, GLfloat* data)
{
    *data = postEventAndQuery<&getParameter>(0.0, pname);
}

QWEBGL_FUNCTION(getFramebufferAttachmentParameteriv, void, glGetFramebufferAttachmentParameteriv,
                GLenum target, GLenum attachment, GLenum pname, GLint *params)
{
    *params = postEventAndQuery<&getFramebufferAttachmentParameteriv>(0, target, attachment, pname);
}

QWEBGL_FUNCTION(getProgramInfoLog, void, glGetProgramInfoLog, GLuint program, GLsizei bufSize,
                GLsizei *length, GLchar *infoLog)
{
    auto value = postEventAndQuery<&getProgramInfoLog>(QString(), program, bufSize);
    *length = value.length();
    if (bufSize >= value.length())
        std::memcpy(infoLog, value.constData(), value.length());
}

QWEBGL_FUNCTION(getProgramiv, void, glGetProgramiv, GLuint program, GLenum pname, GLint *params)
{
    *params = postEventAndQuery<&getProgramiv>(0, program, pname);
}

QWEBGL_FUNCTION(getRenderbufferParameteriv, void, glGetRenderbufferParameteriv, GLenum target,
                GLenum pname, GLint *params)
{
    *params = postEventAndQuery<&getRenderbufferParameteriv>(0, target, pname);
}

QWEBGL_FUNCTION(getShaderInfoLog, void, glGetShaderInfoLog, GLuint shader, GLsizei bufSize,
                GLsizei *length, GLchar *infoLog)
{
    const auto value = postEventAndQuery<&getShaderInfoLog>(QString(), shader, bufSize);
    *length = value.length();
    if (bufSize >= value.length())
        std::memcpy(infoLog, value.constData(), value.length());
}

QWEBGL_FUNCTION(getShaderPrecisionFormat, void, glGetShaderPrecisionFormat, GLenum shadertype,
                GLenum precisiontype, GLint *range, GLint *precision)
{
    const auto value = postEventAndQuery<&getShaderPrecisionFormat>(QVariantMap(), shadertype,
                                                                   precisiontype);
    bool ok;
    range[0] = value[QStringLiteral("rangeMin")].toInt(&ok);
    if (!ok)
        qCCritical(lc, "Invalid rangeMin value");
    range[1] = value[QStringLiteral("rangeMax")].toInt(&ok);
    if (!ok)
        qCCritical(lc, "Invalid rangeMax value");
    *precision = value[QStringLiteral("precision")].toInt(&ok);
    if (!ok)
        qCCritical(lc, "Invalid precision value");
}

QWEBGL_FUNCTION(getShaderSource, void, glGetShaderSource, GLuint shader, GLsizei bufSize,
                GLsizei *length, GLchar *source)
{
    const auto value = postEventAndQuery<&getShaderSource>(QString(), shader, bufSize);
    *length = value.length();
    if (bufSize >= value.length())
        std::memcpy(source, value.constData(), value.length());
}

QWEBGL_FUNCTION(getShaderiv, void, glGetShaderiv, GLuint shader, GLenum pname, GLint *params)
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
    *params = postEventAndQuery<&getShaderiv>(0, shader, pname);
}

QWEBGL_FUNCTION(getTexParameterfv, void, glGetTexParameterfv, GLenum target, GLenum pname,
                GLfloat *params)
{
    *params = postEventAndQuery<&getTexParameterfv>(0.f, target, pname);
}

QWEBGL_FUNCTION(getTexParameteriv, void, glGetTexParameteriv, GLenum target, GLenum pname,
                GLint *params)
{
    *params = postEventAndQuery<&getTexParameteriv>(0, target, pname);
}

QWEBGL_FUNCTION(getUniformLocation, GLint, glGetUniformLocation, GLuint program, const GLchar *name)
{
    return postEventAndQuery<&getUniformLocation>(-1, program, name);
}

QWEBGL_FUNCTION(getUniformfv, void, glGetUniformfv, GLuint program, GLint location,
                 GLfloat *params)
{
    *params = postEventAndQuery<&getUniformfv>(0.f, program, location);
}

QWEBGL_FUNCTION(getUniformiv, void, glGetUniformiv, GLuint program, GLint location, GLint *params)
{
    *params = postEventAndQuery<&getUniformiv>(0, program, location);
}

QWEBGL_FUNCTION(getVertexAttribPointerv, void, glGetVertexAttribPointerv, GLuint index,
                GLenum pname, void **pointer)
{
    Q_UNUSED(index);
    Q_UNUSED(pname);
    Q_UNUSED(pointer);
    qFatal("glGetVertexAttribPointerv not supported");
    return;
}

QWEBGL_FUNCTION(getVertexAttribfv, void, glGetVertexAttribfv, GLuint index, GLenum pname,
                GLfloat *params)
{
    *params = postEventAndQuery<&getVertexAttribfv>(0.f, index, pname);
}

QWEBGL_FUNCTION(getVertexAttribiv, void, glGetVertexAttribiv, GLuint index, GLenum pname,
                GLint *params)
{
    *params = postEventAndQuery<&getVertexAttribiv>(0, index, pname);
}

QWEBGL_FUNCTION(hint, void, glHint, GLenum target, GLenum mode)
{
    postEvent<&hint>(target, mode);
}

QWEBGL_FUNCTION(isBuffer, GLboolean, glIsBuffer, GLuint buffer)
{
    return postEventAndQuery<&isBuffer>(GL_FALSE, buffer);
}

QWEBGL_FUNCTION(isEnabled, GLboolean, glIsEnabled, GLenum cap)
{
    return postEventAndQuery<&isEnabled>(GL_FALSE, cap);
}

QWEBGL_FUNCTION(isFramebuffer, GLboolean, glIsFramebuffer, GLuint framebuffer)
{
    return postEventAndQuery<&isFramebuffer>(GL_FALSE, framebuffer);
}

QWEBGL_FUNCTION(isProgram, GLboolean, glIsProgram, GLuint program)
{
    return postEventAndQuery<&isProgram>(GL_FALSE, program);
}

QWEBGL_FUNCTION(isRenderbuffer, GLboolean, glIsRenderbuffer, GLuint renderbuffer)
{
    return postEventAndQuery<&isRenderbuffer>(GL_FALSE, renderbuffer);
}

QWEBGL_FUNCTION(isShader, GLboolean, glIsShader, GLuint shader)
{
    return postEventAndQuery<&isShader>(GL_FALSE, shader);
}

QWEBGL_FUNCTION(isTexture, GLboolean, glIsTexture, GLuint texture)
{
    return postEventAndQuery<&isTexture>(GL_FALSE, texture);
}

QWEBGL_FUNCTION(lineWidth, void, glLineWidth, GLfloat width)
{
    postEvent<&lineWidth>(width);
}

QWEBGL_FUNCTION(linkProgram, void, glLinkProgram, GLuint program)
{
    postEvent<&linkProgram>(program);
}

QWEBGL_FUNCTION(pixelStorei, void, glPixelStorei, GLenum pname, GLint param)
{
    postEvent<&pixelStorei>(pname, param);
    switch (pname) {
    case GL_UNPACK_ALIGNMENT: currentContextData()->unpackAlignment = param; break;
    }
}

QWEBGL_FUNCTION(polygonOffset, void, glPolygonOffset, GLfloat factor, GLfloat units)
{
    postEvent<&polygonOffset>(factor, units);
}

QWEBGL_FUNCTION(readPixels, void, glReadPixels, GLint x, GLint y, GLsizei width, GLsizei height,
                GLenum format, GLenum type, void * pixels)
{
    const auto value = postEventAndQuery<&readPixels>(QByteArray(), x, y, width, height, format,
                                                     type);
    if (!value.isEmpty())
        std::memcpy(pixels, value.constData(), value.size());
}

QWEBGL_FUNCTION(releaseShaderCompiler, void, glReleaseShaderCompiler)
{
    postEvent<&releaseShaderCompiler>();
}

QWEBGL_FUNCTION(renderbufferStorage, void, glRenderbufferStorage, GLenum target,
                GLenum internalformat, GLsizei width, GLsizei height)
{
    postEvent<&renderbufferStorage>(target, internalformat, width, height);
}

QWEBGL_FUNCTION(sampleCoverage, void, glSampleCoverage, GLfloat value, GLboolean invert)
{
    postEvent<&sampleCoverage>(value, invert);
}

QWEBGL_FUNCTION(scissor, void, glScissor, GLint x, GLint y, GLsizei width, GLsizei height)
{
    postEvent<&scissor>(x, y, width, height);
}

QWEBGL_FUNCTION(shaderBinary, void, glShaderBinary, GLsizei, const GLuint *, GLenum, const void *,
                GLsizei)
{
    qFatal("WebGL does not allow precompiled shaders");
}

QWEBGL_FUNCTION(shaderSource, void, glShaderSource, GLuint shader, GLsizei count,
                const GLchar *const *string, const GLint *length)
{
    auto event = currentContext()->createEvent(QStringLiteral("shaderSource"));
    if (!event)
        return;
    event->addParameters(shader, count);
    for (int i = 0; i < count; ++i) {
        if (!length)
            event->addString(QString::fromLatin1(string[i]));
        else
            event->addString(QString::fromLatin1(string[i], length[i]));
    }
    QCoreApplication::postEvent(QWebGLIntegrationPrivate::instance()->webSocketServer, event);
}

QWEBGL_FUNCTION(stencilFunc, void, glStencilFunc, GLenum func, GLint ref, GLuint mask)
{
    postEvent<&stencilFunc>(func, ref, mask);
}

QWEBGL_FUNCTION(stencilFuncSeparate, void, glStencilFuncSeparate, GLenum face, GLenum func,
                GLint ref, GLuint mask)
{
    postEvent<&stencilFuncSeparate>(face, func, ref, mask);
}

QWEBGL_FUNCTION(stencilMask, void, glStencilMask, GLuint mask)
{
    postEvent<&stencilMask>(mask);
}

QWEBGL_FUNCTION(stencilMaskSeparate, void, glStencilMaskSeparate, GLenum face, GLuint mask)
{
    postEvent<&stencilMaskSeparate>(face, mask);
}

QWEBGL_FUNCTION(stencilOp, void, glStencilOp, GLenum fail, GLenum zfail, GLenum zpass)
{
    postEvent<&stencilOp>(fail, zfail, zpass);
}

QWEBGL_FUNCTION(stencilOpSeparate, void, glStencilOpSeparate, GLenum face, GLenum sfail,
                GLenum dpfail, GLenum dppass)
{
    postEvent<&stencilOpSeparate>(face, sfail, dpfail, dppass);
}

QWEBGL_FUNCTION(texImage2D, void,  glTexImage2D, GLenum target, GLint level, GLint internalformat,
                GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                const void *pixels)
{
    postEvent<&texImage2D>(target, level, internalformat, width, height, border, format, type,
                          pixels ? QByteArray((const char*)pixels,
                                              imageSize(width, height, format, type,
                                                        currentContextData()->pixelStorage))
                                 : nullptr);
}

QWEBGL_FUNCTION(texParameterf, void, glTexParameterf, GLenum target, GLenum pname, GLfloat param)
{
    postEvent<&texParameterf>(target, pname, param);
}

QWEBGL_FUNCTION(texParameterfv, void, glTexParameterfv, GLenum, GLenum, const GLfloat *)
{
    qFatal("glTexParameterfv not implemented");
}

QWEBGL_FUNCTION(texParameteri, void, glTexParameteri, GLenum target, GLenum pname, GLint param)
{
    postEvent<&texParameteri>(target, pname, param);
}

QWEBGL_FUNCTION(texParameteriv, void, glTexParameteriv, GLenum, GLenum, const GLint *)
{
    qFatal("glTexParameteriv not implemented");
}

QWEBGL_FUNCTION(texSubImage2D, void, glTexSubImage2D, GLenum target, GLint level, GLint xoffset,
                GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                const void *pixels)
{
    postEvent<&texSubImage2D>(target, level, xoffset, yoffset, width, height, format, type,
                             pixels ? QByteArray((const char *)pixels,
                                                 imageSize(width, height, format, type,
                                                           currentContextData()->pixelStorage))
                                    : nullptr);
}

QWEBGL_FUNCTION(uniform1f, void, glUniform1f, GLint location, GLfloat v0)
{
    postEvent<&uniform1f>(location, v0);
}

QWEBGL_FUNCTION(uniform1fv, void, glUniform1fv, GLint location, GLsizei count, const GLfloat *value)
{
    postEvent<&uniform1fv>(location, count, qMakePair(value, count));
}

QWEBGL_FUNCTION(uniform1i, void, glUniform1i, GLint location, GLint v0)
{
    postEvent<&uniform1i>(location, v0);
}

QWEBGL_FUNCTION(uniform1iv, void, glUniform1iv, GLint location, GLsizei count, const GLint *value)
{
    postEvent<&uniform1iv>(location, count, qMakePair(value, count));
}

QWEBGL_FUNCTION(uniform2f, void, glUniform2f, GLint location, GLfloat v0, GLfloat v1)
{
    postEvent<&uniform2f>(location, v0, v1);
}

QWEBGL_FUNCTION(uniform2fv, void, glUniform2fv, GLint location, GLsizei count, const GLfloat *value)
{
    postEvent<&uniform2fv>(location, count, qMakePair(value, count * 2));
}

QWEBGL_FUNCTION(uniform2i, void, glUniform2i, GLint location, GLint v0, GLint v1)
{
    postEvent<&uniform2i>(location, v0, v1);
}

QWEBGL_FUNCTION(uniform2iv, void, glUniform2iv, GLint location, GLsizei count, const GLint *value)
{
    postEvent<&uniform2iv>(location, count, qMakePair(value, count * 2));
}

QWEBGL_FUNCTION(uniform3f, void, glUniform3f, GLint location, GLfloat v0, GLfloat v1, GLfloat v2)
{
    postEvent<&uniform3f>(location, v0, v1, v2);
}

QWEBGL_FUNCTION(uniform3fv, void, glUniform3fv, GLint location, GLsizei count, const GLfloat *value)
{
    postEvent<&uniform3fv>(location, count, qMakePair(value, count * 3));
}

QWEBGL_FUNCTION(uniform3i, void, glUniform3i, GLint location, GLint v0, GLint v1, GLint v2)
{
    postEvent<&uniform3i>(location, v0, v1, v2);
}

QWEBGL_FUNCTION(uniform3iv, void, glUniform3iv, GLint location, GLsizei count, const GLint *value)
{
    postEvent<&uniform3iv>(location, count, qMakePair(value, count * 3));
}

QWEBGL_FUNCTION(uniform4f, void, glUniform4f, GLint location, GLfloat v0, GLfloat v1, GLfloat v2,
                GLfloat v3)
{
    postEvent<&uniform4f>(location, v0, v1, v2, v3);
}

QWEBGL_FUNCTION(uniform4fv, void, glUniform4fv, GLint location, GLsizei count, const GLfloat *value)
{
    postEvent<&uniform4fv>(location, count, qMakePair(value, count * 4));
}

QWEBGL_FUNCTION(uniform4i, void, glUniform4i, GLint location, GLint v0, GLint v1, GLint v2,
                GLint v3)
{
    postEvent<&uniform4i>(location, v0, v1, v2, v3);
}

QWEBGL_FUNCTION(uniform4iv, void, glUniform4iv, GLint location, GLsizei count, const GLint *value)
{
    postEvent<&uniform4iv>(location, count, qMakePair(value, count * 4));
}

QWEBGL_FUNCTION(uniformMatrix2fv, void, glUniformMatrix2fv, GLint location, GLsizei count,
                GLboolean transpose, const GLfloat *value)
{
    postEvent<&uniformMatrix2fv>(location, count, transpose, qMakePair(value, count * 4));
}

QWEBGL_FUNCTION(uniformMatrix3fv, void, glUniformMatrix3fv, GLint location, GLsizei count,
                GLboolean transpose, const GLfloat *value)
{
    postEvent<&uniformMatrix3fv>(location, count, transpose, qMakePair(value, count * 9));
}

QWEBGL_FUNCTION(uniformMatrix4fv, void, glUniformMatrix4fv, GLint location, GLsizei count,
                GLboolean transpose, const GLfloat *value)
{
    postEvent<&uniformMatrix4fv>(location, count, transpose, qMakePair(value, count * 16));
}

QWEBGL_FUNCTION(useProgram, void, glUseProgram, GLuint program)
{
    postEvent<&useProgram>(program);
}

QWEBGL_FUNCTION(validateProgram, void, glValidateProgram, GLuint program)
{
    postEvent<&validateProgram>(program);
}

QWEBGL_FUNCTION(vertexAttrib1f, void, glVertexAttrib1f, GLuint index, GLfloat x)
{
    postEvent<&vertexAttrib1f>(index, x);
}

QWEBGL_FUNCTION(vertexAttrib1fv, void, glVertexAttrib1fv, GLuint index, const GLfloat *v)
{
    postEvent<&vertexAttrib1fv>(index, v[0]);
}

QWEBGL_FUNCTION(vertexAttrib2f, void, glVertexAttrib2f, GLuint index, GLfloat x, GLfloat y)
{
    postEvent<&vertexAttrib2f>(index, x, y);
}

QWEBGL_FUNCTION(vertexAttrib2fv, void, glVertexAttrib2fv, GLuint index, const GLfloat *v)
{
    postEvent<&vertexAttrib2fv>(index, v[0], v[1]);
}

QWEBGL_FUNCTION(vertexAttrib3f, void, glVertexAttrib3f, GLuint index, GLfloat x, GLfloat y,
                 GLfloat z)
{
    postEvent<&vertexAttrib3f>(index, x, y, z);
}

QWEBGL_FUNCTION(vertexAttrib3fv, void, glVertexAttrib3fv, GLuint index, const GLfloat *v)
{
    postEvent<&vertexAttrib3fv>(index, v[0], v[1], v[2]);
}

QWEBGL_FUNCTION(vertexAttrib4f, void, glVertexAttrib4f, GLuint index, GLfloat x, GLfloat y,
                 GLfloat z, GLfloat w)
{
    postEvent<&vertexAttrib4f>(index, x, y, z, w);
}

QWEBGL_FUNCTION(vertexAttrib4fv, void, glVertexAttrib4fv, GLuint index, const GLfloat *v)
{
    postEvent<&vertexAttrib4fv>(index, v[0], v[1], v[2], v[3]);
}

QWEBGL_FUNCTION(vertexAttribPointer, void, glVertexAttribPointer, GLuint index, GLint size,
                GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)
{
    ContextData *d = currentContextData();
    ContextData::VertexAttrib &va(d->vertexAttribPointers[index]);
    va.arrayBufferBinding = d->boundArrayBuffer;
    va.size = size;
    va.type = type;
    va.normalized = normalized;
    va.stride = stride;
    va.pointer = (void *) pointer;
    if (d->boundArrayBuffer)
        postEvent<&vertexAttribPointer>(index, size, type, normalized, stride,
                                       uint(quintptr(pointer)));
}

QWEBGL_FUNCTION(viewport, void, glViewport, GLint x, GLint y, GLsizei width, GLsizei height)
{
    postEvent<&viewport>(x, y, width, height);
    auto it = currentContextData()->cachedParameters.find(GL_VIEWPORT);
    if (it != currentContextData()->cachedParameters.end())
        it->setValue(QVariantList{ x, y, width, height });
}

QWEBGL_FUNCTION(blitFramebufferEXT, void, glBlitFramebufferEXT, GLint srcX0, GLint srcY0,
                 GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                 GLbitfield mask, GLenum filter)
{
    postEvent<&blitFramebufferEXT>(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask,
                                  filter);
}

QWEBGL_FUNCTION(renderbufferStorageMultisampleEXT, void, glRenderbufferStorageMultisampleEXT,
                GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                GLsizei height)
{
    postEvent<&renderbufferStorageMultisampleEXT>(target, samples, internalformat, width, height);
}

QWEBGL_FUNCTION(getTexLevelParameteriv, void, glGetTexLevelParameteriv, GLenum, GLint, GLenum,
                GLint *)
{
    qFatal("glGetTexLevelParameteriv not supported");
}

#undef QWEBGL_FUNCTION

extern const GLFunction makeCurrent("makeCurrent");
extern const GLFunction swapBuffers("swapBuffers");

}

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
    auto event = createEvent(QStringLiteral("swapBuffers"), true);
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
    auto event = createEvent(QStringLiteral("makeCurrent"));
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
    postEvent<&QWebGL::makeCurrent>(0, 0, 0, 0);
}

bool QWebGLContext::isValid() const
{
    Q_D(const QWebGLContext);
    return d->id != -1;
}

QFunctionPointer QWebGLContext::getProcAddress(const char *procName)
{
    const auto it = glFunctions.find(procName);
    return it != glFunctions.end() ? (*it)->functionPointer : nullptr;
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
    if (wait)
        QWebGLContextPrivate::waitingIds.insert(pointer->id());

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
