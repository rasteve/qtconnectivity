/*
 * This file was generated by qdbusxml2cpp version 0.8
 * Command line was: qdbusxml2cpp -I QtCore/private/qglobal_p.h -p gattchar1_p.h:gattchar1.cpp org.bluez.GattCharacteristic1.xml
 *
 * qdbusxml2cpp is Copyright (C) 2022 The Qt Company Ltd.
 *
 * This is an auto-generated file.
 * Do not edit! All changes made to it will be lost.
 */

#ifndef GATTCHAR1_P_H
#define GATTCHAR1_P_H

#include <QtCore/QObject>
#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtDBus/QtDBus>
#include <QtCore/private/qglobal_p.h>

/*
 * Proxy class for interface org.bluez.GattCharacteristic1
 */
class OrgBluezGattCharacteristic1Interface: public QDBusAbstractInterface
{
    Q_OBJECT
public:
    static inline const char *staticInterfaceName()
    { return "org.bluez.GattCharacteristic1"; }

public:
    OrgBluezGattCharacteristic1Interface(const QString &service, const QString &path, const QDBusConnection &connection, QObject *parent = nullptr);

    ~OrgBluezGattCharacteristic1Interface();

    Q_PROPERTY(QStringList Flags READ flags)
    inline QStringList flags() const
    { return qvariant_cast< QStringList >(property("Flags")); }

    Q_PROPERTY(bool Notifying READ notifying)
    inline bool notifying() const
    { return qvariant_cast< bool >(property("Notifying")); }

    Q_PROPERTY(QDBusObjectPath Service READ service)
    inline QDBusObjectPath service() const
    { return qvariant_cast< QDBusObjectPath >(property("Service")); }

    Q_PROPERTY(QString UUID READ uUID)
    inline QString uUID() const
    { return qvariant_cast< QString >(property("UUID")); }

    Q_PROPERTY(QByteArray Value READ value)
    inline QByteArray value() const
    { return qvariant_cast< QByteArray >(property("Value")); }

public Q_SLOTS: // METHODS
    inline QDBusPendingReply<QByteArray> ReadValue(const QVariantMap &options)
    {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(options);
        return asyncCallWithArgumentList(QStringLiteral("ReadValue"), argumentList);
    }

    inline QDBusPendingReply<> StartNotify()
    {
        QList<QVariant> argumentList;
        return asyncCallWithArgumentList(QStringLiteral("StartNotify"), argumentList);
    }

    inline QDBusPendingReply<> StopNotify()
    {
        QList<QVariant> argumentList;
        return asyncCallWithArgumentList(QStringLiteral("StopNotify"), argumentList);
    }

    inline QDBusPendingReply<> WriteValue(const QByteArray &value, const QVariantMap &options)
    {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(value) << QVariant::fromValue(options);
        return asyncCallWithArgumentList(QStringLiteral("WriteValue"), argumentList);
    }

Q_SIGNALS: // SIGNALS
};

namespace org {
  namespace bluez {
    using GattCharacteristic1 = ::OrgBluezGattCharacteristic1Interface;
  }
}
#endif
