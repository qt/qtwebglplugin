/****************************************************************************
** Meta object code from reading C++ file 'qwebglwebsocketserver.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.10.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../qwebglwebsocketserver.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'qwebglwebsocketserver.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.10.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_QWebGLWebSocketServer_t {
    QByteArrayData data[13];
    char stringdata0[167];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_QWebGLWebSocketServer_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_QWebGLWebSocketServer_t qt_meta_stringdata_QWebGLWebSocketServer = {
    {
QT_MOC_LITERAL(0, 0, 21), // "QWebGLWebSocketServer"
QT_MOC_LITERAL(1, 22, 6), // "create"
QT_MOC_LITERAL(2, 29, 0), // ""
QT_MOC_LITERAL(3, 30, 11), // "sendMessage"
QT_MOC_LITERAL(4, 42, 11), // "QWebSocket*"
QT_MOC_LITERAL(5, 54, 6), // "socket"
QT_MOC_LITERAL(6, 61, 34), // "QWebGLWebSocketServer::Messag..."
QT_MOC_LITERAL(7, 96, 4), // "type"
QT_MOC_LITERAL(8, 101, 6), // "values"
QT_MOC_LITERAL(9, 108, 15), // "onNewConnection"
QT_MOC_LITERAL(10, 124, 12), // "onDisconnect"
QT_MOC_LITERAL(11, 137, 21), // "onTextMessageReceived"
QT_MOC_LITERAL(12, 159, 7) // "message"

    },
    "QWebGLWebSocketServer\0create\0\0sendMessage\0"
    "QWebSocket*\0socket\0"
    "QWebGLWebSocketServer::MessageType\0"
    "type\0values\0onNewConnection\0onDisconnect\0"
    "onTextMessageReceived\0message"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_QWebGLWebSocketServer[] = {

 // content:
       7,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    0,   39,    2, 0x0a /* Public */,
       3,    3,   40,    2, 0x0a /* Public */,
       9,    0,   47,    2, 0x08 /* Private */,
      10,    0,   48,    2, 0x08 /* Private */,
      11,    1,   49,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 4, 0x80000000 | 6, QMetaType::QVariantMap,    5,    7,    8,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   12,

       0        // eod
};

void QWebGLWebSocketServer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        QWebGLWebSocketServer *_t = static_cast<QWebGLWebSocketServer *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->create(); break;
        case 1: _t->sendMessage((*reinterpret_cast< QWebSocket*(*)>(_a[1])),(*reinterpret_cast< QWebGLWebSocketServer::MessageType(*)>(_a[2])),(*reinterpret_cast< const QVariantMap(*)>(_a[3]))); break;
        case 2: _t->onNewConnection(); break;
        case 3: _t->onDisconnect(); break;
        case 4: _t->onTextMessageReceived((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    }
}

const QMetaObject QWebGLWebSocketServer::staticMetaObject = {
    { &QObject::staticMetaObject, qt_meta_stringdata_QWebGLWebSocketServer.data,
      qt_meta_data_QWebGLWebSocketServer,  qt_static_metacall, nullptr, nullptr}
};


const QMetaObject *QWebGLWebSocketServer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QWebGLWebSocketServer::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_QWebGLWebSocketServer.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int QWebGLWebSocketServer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
