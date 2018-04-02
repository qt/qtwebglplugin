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

#ifndef QWEBGLCURSOR_H
#define QWEBGLCURSOR_h

#include <QtGui/qpa/qplatformcursor.h>

QT_BEGIN_NAMESPACE

class QWebGLScreen;

<<<<<<< HEAD
class QWebGLCursor : public QPlatformCursor, public QObject
{
    Q_OBJECT
public:
    QWebGLCursor(QWebGLScreen *screen/*, QObject *htmlService*/);
=======
class QWebGLCursor : public QPlatformCursor
{
    Q_OBJECT
public:
    QWebGLCursor();
>>>>>>> 7b9234e90f74a46fd6af89d5ab39aec0d8537b3f
    
    void changeCursor(QCursor *windowCursor, QWindow *window);

    Q_SIGNALS: 
    void cursorChanged(QCursor *windowCursor);
<<<<<<< HEAD

=======
private:
    QObject *mHtmlService;
>>>>>>> 7b9234e90f74a46fd6af89d5ab39aec0d8537b3f
};

QT_END_NAMESPACE

#endif
