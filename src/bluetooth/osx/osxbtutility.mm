/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qbluetoothaddress.h"
#include "osxbtutility_p.h"
#include "qbluetoothuuid.h"

#include <QtCore/qstring.h>

#ifndef QT_IOS_BLUETOOTH

#import <IOBluetooth/objc/IOBluetoothSDPUUID.h>

#endif

#include <algorithm>
#include <limits>

QT_BEGIN_NAMESPACE

#ifndef QT_IOS_BLUETOOTH

Q_LOGGING_CATEGORY(QT_BT_OSX, "qt.bluetooth.osx")

#else

Q_LOGGING_CATEGORY(QT_BT_OSX, "qt.bluetooth.ios")

#endif

namespace OSXBluetooth {

QString qt_address(NSString *address)
{
    if (address && address.length) {
        NSString *const fixed = [address stringByReplacingOccurrencesOfString:@"-" withString:@":"];
        return QString::fromNSString(fixed);
    }

    return QString();
}

#ifndef QT_IOS_BLUETOOTH


QBluetoothAddress qt_address(const BluetoothDeviceAddress *a)
{
    if (a) {
        // TODO: can a byte order be different in BluetoothDeviceAddress?
        const quint64 qAddress = a->data[5] |
                                 qint64(a->data[4]) << 8  |
                                 qint64(a->data[3]) << 16 |
                                 qint64(a->data[2]) << 24 |
                                 qint64(a->data[1]) << 32 |
                                 qint64(a->data[0]) << 40;
        return QBluetoothAddress(qAddress);
    }

    return QBluetoothAddress();
}

BluetoothDeviceAddress iobluetooth_address(const QBluetoothAddress &qAddress)
{
    BluetoothDeviceAddress a = {};
    if (!qAddress.isNull()) {
        const quint64 val = qAddress.toUInt64();
        a.data[0] = (val >> 40) & 0xff;
        a.data[1] = (val >> 32) & 0xff;
        a.data[2] = (val >> 24) & 0xff;
        a.data[3] = (val >> 16) & 0xff;
        a.data[4] = (val >> 8) & 0xff;
        a.data[5] = val & 0xff;
    }

    return a;
}

ObjCStrongReference<IOBluetoothSDPUUID> iobluetooth_uuid(const QBluetoothUuid &uuid)
{
    const unsigned nBytes = 128 / std::numeric_limits<unsigned char>::digits;
    const quint128 intVal(uuid.toUInt128());

    const ObjCStrongReference<IOBluetoothSDPUUID> iobtUUID([IOBluetoothSDPUUID uuidWithBytes:intVal.data
                                                           length:nBytes], true);
    return iobtUUID;
}

QBluetoothUuid qt_uuid(IOBluetoothSDPUUID *uuid)
{
    QBluetoothUuid qtUuid;
    if (!uuid || [uuid length] != 16) // TODO: issue any diagnostic?
        return qtUuid;

    // TODO: ensure the correct byte-order!!!
    quint128 uuidVal = {};
    const quint8 *const source = static_cast<const quint8 *>([uuid bytes]);
    std::copy(source, source + 16, uuidVal.data);
    return QBluetoothUuid(uuidVal);
}

QString qt_error_string(IOReturn errorCode)
{
    switch (errorCode) {
    case kIOReturnSuccess:
        // NoError in many classes == an empty string description.
        return QString();
    case kIOReturnNoMemory:
        return QString::fromLatin1("memory allocation failed");
    case kIOReturnNoResources:
        return QString::fromLatin1("failed to obtain a resource");
    case kIOReturnBusy:
        return QString::fromLatin1("device is busy");
    case kIOReturnStillOpen:
        return QString::fromLatin1("device(s) still open");
    // Others later ...
    case kIOReturnError: // "general error" (IOReturn.h)
    default:
        return QString::fromLatin1("unknown error");
    }
}

#endif


// Apple has: CBUUID, NSUUID, CFUUID, IOBluetoothSDPUUID
// and it's handy to have several converters:

QBluetoothUuid qt_uuid(CBUUID *uuid)
{
    // Apples' docs say "128 bit" and "16-bit UUIDs are implicitly
    // pre-filled with the Bluetooth Base UUID."
    // But Core Bluetooth can return CBUUID objects of length 2
    // (16-bit, so they are not pre-filled?).

    if (!uuid)
        return QBluetoothUuid();

    QT_BT_MAC_AUTORELEASEPOOL;

    if (uuid.data.length == 2) {
        // TODO: this is .. UGLY :)
        quint16 qtUuidData = 0;
        const quint8 *const source = static_cast<const quint8 *>(uuid.data.bytes);
        std::copy(source, source + 2, &qtUuidData);

        return QBluetoothUuid(qtUuidData);
    } else if (uuid.data.length == 16) {
        quint128 qtUuidData = {};
        const quint8 *const source = static_cast<const quint8 *>(uuid.data.bytes);
        std::copy(source, source + 16, qtUuidData.data);

        return QBluetoothUuid(qtUuidData);
    } else {
        qCDebug(QT_BT_OSX) << "qt_uuid, invalid CBUUID, 2 or 16 bytes expected, but got "
                           << uuid.data.length << " bytes length";
        return QBluetoothUuid();
    }

    if (uuid.data.length != 16) // TODO: warning?
        return QBluetoothUuid();

}

CFStrongReference<CFUUIDRef> cf_uuid(const QBluetoothUuid &qtUuid)
{
    const quint128 qtUuidData = qtUuid.toUInt128();
    const quint8 *const data = qtUuidData.data;

    CFUUIDBytes bytes = {data[0],  data[1],  data[2],  data[3],
                         data[4],  data[5],  data[6],  data[7],
                         data[8],  data[9],  data[10], data[11],
                         data[12], data[13], data[14], data[15]};

    CFUUIDRef cfUuid = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, bytes);
    return CFStrongReference<CFUUIDRef>(cfUuid, false);// false == already retained.
}

ObjCStrongReference<CBUUID> cb_uuid(const QBluetoothUuid &qtUuid)
{
    CFStrongReference<CFUUIDRef> cfUuid(cf_uuid(qtUuid));
    if (!cfUuid)
        return ObjCStrongReference<CBUUID>();

    ObjCStrongReference<CBUUID> cbUuid([CBUUID UUIDWithCFUUID:cfUuid], true); //true == retain.
    return cbUuid;
}

}

QT_END_NAMESPACE
