/*
 * This file was generated by dbusidl2cpp version 0.3
 * when processing input file /home/tjmaciei/src/kde4/playground/libs/qt-dbus/examples/com.trolltech.ChatInterface.xml
 *
 * dbusidl2cpp is Copyright (C) 2006 Trolltech AS. All rights reserved.
 *
 * This is an auto-generated file.
 */

#include "chatadaptor.h"
#include <QtCore/QMetaObject>
#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

/*
 * Implementation of adaptor class ChatInterfaceAdaptor
 */

ChatInterfaceAdaptor::ChatInterfaceAdaptor(QObject *parent)
   : QDBusAbstractAdaptor(parent)
{
    // constructor
    setAutoRelaySignals(true);
}

ChatInterfaceAdaptor::~ChatInterfaceAdaptor()
{
    // destructor
}


#include "chatadaptor.moc"
