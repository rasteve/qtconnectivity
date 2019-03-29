/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qlowenergycontroller_winrt_new_p.h"
#include "qlowenergycontroller_winrt_p.h"

#include <QtBluetooth/QLowEnergyCharacteristicData>
#include <QtBluetooth/QLowEnergyDescriptorData>
#include <QtBluetooth/private/qbluetoothutils_winrt_p.h>

#ifdef CLASSIC_APP_BUILD
#define Q_OS_WINRT
#endif
#include <QtCore/qfunctions_winrt.h>
#include <QtCore/QtEndian>
#include <QtCore/QLoggingCategory>
#include <private/qeventdispatcher_winrt_p.h>

#include <functional>
#include <robuffer.h>
#include <windows.devices.enumeration.h>
#include <windows.devices.bluetooth.h>
#include <windows.foundation.collections.h>
#include <windows.foundation.metadata.h>
#include <windows.storage.streams.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Foundation::Collections;
using namespace ABI::Windows::Foundation::Metadata;
using namespace ABI::Windows::Devices;
using namespace ABI::Windows::Devices::Bluetooth;
using namespace ABI::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace ABI::Windows::Devices::Enumeration;
using namespace ABI::Windows::Storage::Streams;

QT_BEGIN_NAMESPACE

typedef ITypedEventHandler<BluetoothLEDevice *, IInspectable *> StatusHandler;
typedef ITypedEventHandler<GattCharacteristic *, GattValueChangedEventArgs *> ValueChangedHandler;
typedef GattReadClientCharacteristicConfigurationDescriptorResult ClientCharConfigDescriptorResult;
typedef IGattReadClientCharacteristicConfigurationDescriptorResult IClientCharConfigDescriptorResult;

#define EMIT_WORKER_ERROR_AND_QUIT_IF_FAILED(hr, ret) \
    if (FAILED(hr)) { \
        emitErrorAndQuitThread(hr); \
        ret; \
    }

#define WARN_AND_CONTINUE_IF_FAILED(hr, msg) \
    if (FAILED(hr)) { \
        qCWarning(QT_BT_WINRT) << msg; \
        continue; \
    }

#define CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, msg, ret) \
    if (FAILED(hr)) { \
        qCWarning(QT_BT_WINRT) << msg; \
        setError(QLowEnergyController::ConnectionError); \
        setState(QLowEnergyController::UnconnectedState); \
        ret; \
    }

Q_DECLARE_LOGGING_CATEGORY(QT_BT_WINRT)

QLowEnergyControllerPrivate *createWinRTLowEnergyController()
{
    if (supportsNewLEApi()) {
        qCDebug(QT_BT_WINRT) << "Using new low energy controller";
        return new QLowEnergyControllerPrivateWinRTNew();
    }

    qCDebug(QT_BT_WINRT) << "Using pre 15063 low energy controller";
    return new QLowEnergyControllerPrivateWinRT();
}

static QByteArray byteArrayFromBuffer(const ComPtr<IBuffer> &buffer, bool isWCharString = false)
{
    ComPtr<Windows::Storage::Streams::IBufferByteAccess> byteAccess;
    HRESULT hr = buffer.As(&byteAccess);
    Q_ASSERT_SUCCEEDED(hr);
    char *data;
    hr = byteAccess->Buffer(reinterpret_cast<byte **>(&data));
    Q_ASSERT_SUCCEEDED(hr);
    UINT32 size;
    hr = buffer->get_Length(&size);
    Q_ASSERT_SUCCEEDED(hr);
    if (isWCharString) {
        QString valueString = QString::fromUtf16(reinterpret_cast<ushort *>(data)).left(size / 2);
        return valueString.toUtf8();
    }
    return QByteArray(data, int(size));
}

static QByteArray byteArrayFromGattResult(const ComPtr<IGattReadResult> &gattResult,
                                          bool isWCharString = false)
{
    ComPtr<ABI::Windows::Storage::Streams::IBuffer> buffer;
    HRESULT hr;
    hr = gattResult->get_Value(&buffer);
    Q_ASSERT_SUCCEEDED(hr);
    return byteArrayFromBuffer(buffer, isWCharString);
}

class QWinRTLowEnergyServiceHandlerNew : public QObject
{
    Q_OBJECT
public:
    QWinRTLowEnergyServiceHandlerNew(const QBluetoothUuid &service,
                                     const ComPtr<IGattDeviceService3> &deviceService)
        : mService(service)
        , mDeviceService(deviceService)
    {
        qCDebug(QT_BT_WINRT) << __FUNCTION__;
    }

    ~QWinRTLowEnergyServiceHandlerNew()
    {
    }

public slots:
    void obtainCharList()
    {
        mIndicateChars.clear();
        qCDebug(QT_BT_WINRT) << __FUNCTION__;
        ComPtr<IAsyncOperation<GattCharacteristicsResult *>> characteristicsOp;
        ComPtr<IGattCharacteristicsResult> characteristicsResult;
        HRESULT hr = mDeviceService->GetCharacteristicsAsync(&characteristicsOp);
        EMIT_WORKER_ERROR_AND_QUIT_IF_FAILED(hr, return);
        hr = QWinRTFunctions::await(characteristicsOp, characteristicsResult.GetAddressOf(),
                                    QWinRTFunctions::ProcessMainThreadEvents, 5000);
        EMIT_WORKER_ERROR_AND_QUIT_IF_FAILED(hr, return);
        GattCommunicationStatus status;
        hr = characteristicsResult->get_Status(&status);
        EMIT_WORKER_ERROR_AND_QUIT_IF_FAILED(hr, return);
        if (status != GattCommunicationStatus_Success) {
            emitErrorAndQuitThread(QLatin1String("Could not obtain char list"));
            return;
        }
        ComPtr<IVectorView<GattCharacteristic *>> characteristics;
        hr = characteristicsResult->get_Characteristics(&characteristics);
        EMIT_WORKER_ERROR_AND_QUIT_IF_FAILED(hr, return);

        uint characteristicsCount;
        hr = characteristics->get_Size(&characteristicsCount);
        EMIT_WORKER_ERROR_AND_QUIT_IF_FAILED(hr, return);

        mCharacteristicsCountToBeDiscovered = characteristicsCount;
        for (uint i = 0; i < characteristicsCount; ++i) {
            ComPtr<IGattCharacteristic> characteristic;
            hr = characteristics->GetAt(i, &characteristic);
            if (FAILED(hr)) {
                qCWarning(QT_BT_WINRT) << "Could not obtain characteristic at" << i;
                --mCharacteristicsCountToBeDiscovered;
                continue;
            }

            ComPtr<IGattCharacteristic3> characteristic3;
            hr = characteristic.As(&characteristic3);
            if (FAILED(hr)) {
                qCWarning(QT_BT_WINRT) << "Could not cast characteristic";
                --mCharacteristicsCountToBeDiscovered;
                continue;
            }

            // For some strange reason, Windows doesn't discover descriptors of characteristics (if not paired).
            // Qt API assumes that all characteristics and their descriptors are discovered in one go.
            // So we start 'GetDescriptorsAsync' for each discovered characteristic and finish only
            // when GetDescriptorsAsync for all characteristics return.
            ComPtr<IAsyncOperation<GattDescriptorsResult*>> descAsyncResult;
            hr = characteristic3->GetDescriptorsAsync(&descAsyncResult);
            if (FAILED(hr)) {
                qCWarning(QT_BT_WINRT) << "Could not obtain list of descriptors";
                --mCharacteristicsCountToBeDiscovered;
                continue;
            }
            hr = descAsyncResult->put_Completed(
                        Callback<IAsyncOperationCompletedHandler<GattDescriptorsResult*>>(
                            [this, characteristic]
                            (IAsyncOperation<GattDescriptorsResult *> *op,
                            AsyncStatus status) {
                if (status != AsyncStatus::Completed) {
                    qCWarning(QT_BT_WINRT) << "Descriptor operation unsuccessful";
                    --mCharacteristicsCountToBeDiscovered;
                    checkAllCharacteristicsDiscovered();
                    return S_OK;
                }
                quint16 handle;

                HRESULT hr = characteristic->get_AttributeHandle(&handle);
                if (FAILED(hr)) {
                    qCWarning(QT_BT_WINRT) << "Could not obtain characteristic's attribute handle";
                    --mCharacteristicsCountToBeDiscovered;
                    checkAllCharacteristicsDiscovered();
                    return S_OK;
                }
                QLowEnergyServicePrivate::CharData charData;
                charData.valueHandle = handle + 1;
                if (mStartHandle == 0 || mStartHandle > handle)
                    mStartHandle = handle;
                if (mEndHandle == 0 || mEndHandle < handle)
                    mEndHandle = handle;
                GUID guuid;
                hr = characteristic->get_Uuid(&guuid);
                if (FAILED(hr)) {
                    qCWarning(QT_BT_WINRT) << "Could not obtain characteristic's Uuid";
                    --mCharacteristicsCountToBeDiscovered;
                    checkAllCharacteristicsDiscovered();
                    return S_OK;
                }
                charData.uuid = QBluetoothUuid(guuid);
                GattCharacteristicProperties properties;
                hr = characteristic->get_CharacteristicProperties(&properties);
                if (FAILED(hr)) {
                    qCWarning(QT_BT_WINRT) << "Could not obtain characteristic's properties";
                    --mCharacteristicsCountToBeDiscovered;
                    checkAllCharacteristicsDiscovered();
                    return S_OK;
                }
                charData.properties = QLowEnergyCharacteristic::PropertyTypes(properties & 0xff);
                if (charData.properties & QLowEnergyCharacteristic::Read) {
                    ComPtr<IAsyncOperation<GattReadResult *>> readOp;
                    hr = characteristic->ReadValueWithCacheModeAsync(BluetoothCacheMode_Uncached,
                                                                     &readOp);
                    if (FAILED(hr)) {
                        qCWarning(QT_BT_WINRT) << "Could not read characteristic";
                        --mCharacteristicsCountToBeDiscovered;
                        checkAllCharacteristicsDiscovered();
                        return S_OK;
                    }
                    ComPtr<IGattReadResult> readResult;
                    hr = QWinRTFunctions::await(readOp, readResult.GetAddressOf());
                    if (FAILED(hr)) {
                        qCWarning(QT_BT_WINRT) << "Could not obtain characteristic read result";
                        --mCharacteristicsCountToBeDiscovered;
                        checkAllCharacteristicsDiscovered();
                        return S_OK;
                    }
                    if (!readResult)
                        qCWarning(QT_BT_WINRT) << "Characteristic read result is null";
                    else
                        charData.value = byteArrayFromGattResult(readResult);
                }
                mCharacteristicList.insert(handle, charData);

                ComPtr<IVectorView<GattDescriptor *>> descriptors;

                ComPtr<IGattDescriptorsResult> result;
                hr = op->GetResults(&result);
                if (FAILED(hr)) {
                    qCWarning(QT_BT_WINRT) << "Could not obtain descriptor read result";
                    --mCharacteristicsCountToBeDiscovered;
                    checkAllCharacteristicsDiscovered();
                    return S_OK;
                }
                GattCommunicationStatus commStatus;
                hr = result->get_Status(&commStatus);
                if (FAILED(hr) || commStatus != GattCommunicationStatus_Success) {
                    qCWarning(QT_BT_WINRT) << "Descriptor operation failed";
                    --mCharacteristicsCountToBeDiscovered;
                    checkAllCharacteristicsDiscovered();
                    return S_OK;
                }

                hr = result->get_Descriptors(&descriptors);
                if (FAILED(hr)) {
                    qCWarning(QT_BT_WINRT) << "Could not obtain list of descriptors";
                    --mCharacteristicsCountToBeDiscovered;
                    checkAllCharacteristicsDiscovered();
                    return S_OK;
                }

                uint descriptorCount;
                hr = descriptors->get_Size(&descriptorCount);
                if (FAILED(hr)) {
                    qCWarning(QT_BT_WINRT) << "Could not obtain list of descriptors' size";
                    --mCharacteristicsCountToBeDiscovered;
                    checkAllCharacteristicsDiscovered();
                    return S_OK;
                }
                for (uint j = 0; j < descriptorCount; ++j) {
                    QLowEnergyServicePrivate::DescData descData;
                    ComPtr<IGattDescriptor> descriptor;
                    hr = descriptors->GetAt(j, &descriptor);
                    WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain descriptor")
                    quint16 descHandle;
                    hr = descriptor->get_AttributeHandle(&descHandle);
                    WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain descriptor's attribute handle")
                    GUID descriptorUuid;
                    hr = descriptor->get_Uuid(&descriptorUuid);
                    WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain descriptor's Uuid")
                    descData.uuid = QBluetoothUuid(descriptorUuid);
                    charData.descriptorList.insert(descHandle, descData);
                    if (descData.uuid == QBluetoothUuid(QBluetoothUuid::ClientCharacteristicConfiguration)) {
                        ComPtr<IAsyncOperation<ClientCharConfigDescriptorResult *>> readOp;
                        hr = characteristic->ReadClientCharacteristicConfigurationDescriptorAsync(&readOp);
                        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not read descriptor value")
                        ComPtr<IClientCharConfigDescriptorResult> readResult;
                        hr = QWinRTFunctions::await(readOp, readResult.GetAddressOf());
                        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not await descriptor read result")
                        GattClientCharacteristicConfigurationDescriptorValue value;
                        hr = readResult->get_ClientCharacteristicConfigurationDescriptor(&value);
                        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not get descriptor value from result")
                        quint16 result = 0;
                        bool correct = false;
                        if (value & GattClientCharacteristicConfigurationDescriptorValue_Indicate) {
                            result |= GattClientCharacteristicConfigurationDescriptorValue_Indicate;
                            correct = true;
                        }
                        if (value & GattClientCharacteristicConfigurationDescriptorValue_Notify) {
                            result |= GattClientCharacteristicConfigurationDescriptorValue_Notify;
                            correct = true;
                        }
                        if (value == GattClientCharacteristicConfigurationDescriptorValue_None) {
                            correct = true;
                        }
                        if (!correct)
                            continue;

                        descData.value = QByteArray(2, Qt::Uninitialized);
                        qToLittleEndian(result, descData.value.data());
                        mIndicateChars << charData.uuid;
                    } else {
                        ComPtr<IAsyncOperation<GattReadResult *>> readOp;
                        hr = descriptor->ReadValueWithCacheModeAsync(BluetoothCacheMode_Uncached,
                                                                     &readOp);
                        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not read descriptor value")
                        ComPtr<IGattReadResult> readResult;
                        hr = QWinRTFunctions::await(readOp, readResult.GetAddressOf());
                        WARN_AND_CONTINUE_IF_FAILED(hr, "Could await descriptor read result")
                        if (descData.uuid == QBluetoothUuid::CharacteristicUserDescription)
                            descData.value = byteArrayFromGattResult(readResult, true);
                        else
                            descData.value = byteArrayFromGattResult(readResult);
                    }
                    charData.descriptorList.insert(descHandle, descData);
                }

                mCharacteristicList.insert(handle, charData);
                --mCharacteristicsCountToBeDiscovered;
                checkAllCharacteristicsDiscovered();
                return S_OK;
            }).Get());
            if (FAILED(hr)) {
                qCWarning(QT_BT_WINRT) << "Could not register descriptor callback";
                --mCharacteristicsCountToBeDiscovered;
                continue;
            }
        }
        checkAllCharacteristicsDiscovered();
    }

private:
    bool checkAllCharacteristicsDiscovered();
    void emitErrorAndQuitThread(HRESULT hr);
    void emitErrorAndQuitThread(const QString &error);

public:
    QBluetoothUuid mService;
    ComPtr<IGattDeviceService3> mDeviceService;
    QHash<QLowEnergyHandle, QLowEnergyServicePrivate::CharData> mCharacteristicList;
    uint mCharacteristicsCountToBeDiscovered;
    quint16 mStartHandle = 0;
    quint16 mEndHandle = 0;
    QVector<QBluetoothUuid> mIndicateChars;

signals:
    void charListObtained(const QBluetoothUuid &service, QHash<QLowEnergyHandle,
                          QLowEnergyServicePrivate::CharData> charList,
                          QVector<QBluetoothUuid> indicateChars,
                          QLowEnergyHandle startHandle, QLowEnergyHandle endHandle);
    void errorOccured(const QString &error);
};

bool QWinRTLowEnergyServiceHandlerNew::checkAllCharacteristicsDiscovered()
{
    if (mCharacteristicsCountToBeDiscovered == 0) {
        emit charListObtained(mService, mCharacteristicList, mIndicateChars,
                              mStartHandle, mEndHandle);
        QThread::currentThread()->quit();
        return true;
    }

    return false;
}

void QWinRTLowEnergyServiceHandlerNew::emitErrorAndQuitThread(HRESULT hr)
{
    emitErrorAndQuitThread(qt_error_string(hr));
}

void QWinRTLowEnergyServiceHandlerNew::emitErrorAndQuitThread(const QString &error)
{
    emit errorOccured(error);
    QThread::currentThread()->quit();
}

QLowEnergyControllerPrivateWinRTNew::QLowEnergyControllerPrivateWinRTNew()
    : QLowEnergyControllerPrivate()
{
    registerQLowEnergyControllerMetaType();
}

QLowEnergyControllerPrivateWinRTNew::~QLowEnergyControllerPrivateWinRTNew()
{
    if (mDevice && mStatusChangedToken.value)
        mDevice->remove_ConnectionStatusChanged(mStatusChangedToken);

    unregisterFromValueChanges();
}

void QLowEnergyControllerPrivateWinRTNew::init()
{
}

void QLowEnergyControllerPrivateWinRTNew::connectToDevice()
{
    qCDebug(QT_BT_WINRT) << __FUNCTION__;
    Q_Q(QLowEnergyController);
    if (remoteDevice.isNull()) {
        qWarning() << "Invalid/null remote device address";
        setError(QLowEnergyController::UnknownRemoteDeviceError);
        return;
    }

    setState(QLowEnergyController::ConnectingState);

    ComPtr<IBluetoothLEDeviceStatics> deviceStatics;
    HRESULT hr = GetActivationFactory(
                HString::MakeReference(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice).Get(),
                &deviceStatics);
    CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not obtain device factory", return)
    ComPtr<IAsyncOperation<BluetoothLEDevice *>> deviceFromIdOperation;
    hr = deviceStatics->FromBluetoothAddressAsync(remoteDevice.toUInt64(), &deviceFromIdOperation);
    CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not find LE device from address", return)
    hr = QWinRTFunctions::await(deviceFromIdOperation, mDevice.GetAddressOf(),
                                QWinRTFunctions::ProcessMainThreadEvents, 5000);
    if (FAILED(hr) || !mDevice) {
        qCWarning(QT_BT_WINRT) << "Could not find LE device";
        setError(QLowEnergyController::InvalidBluetoothAdapterError);
        setState(QLowEnergyController::UnconnectedState);
        return;
    }
    BluetoothConnectionStatus status;
    hr = mDevice->get_ConnectionStatus(&status);
    CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not obtain device's connection status", return)
    hr = QEventDispatcherWinRT::runOnXamlThread([this, q]() {
        HRESULT hr;
        hr = mDevice->add_ConnectionStatusChanged(
            Callback<StatusHandler>([this, q](IBluetoothLEDevice *dev, IInspectable *) {
                BluetoothConnectionStatus status;
                HRESULT hr;
                hr = dev->get_ConnectionStatus(&status);
                CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not obtain connection status", return S_OK)
                if (state == QLowEnergyController::ConnectingState
                        && status == BluetoothConnectionStatus::BluetoothConnectionStatus_Connected) {
                    setState(QLowEnergyController::ConnectedState);
                    emit q->connected();
                } else if (state != QLowEnergyController::UnconnectedState
                           && status == BluetoothConnectionStatus::BluetoothConnectionStatus_Disconnected) {
                    invalidateServices();
                    unregisterFromValueChanges();
                    mDevice = nullptr;
                    setError(QLowEnergyController::RemoteHostClosedError);
                    setState(QLowEnergyController::UnconnectedState);
                    emit q->disconnected();
                }
                return S_OK;
            }).Get(), &mStatusChangedToken);
        CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not register connection status callback", return S_OK)
        return S_OK;
    });
    CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not add status callback on Xaml thread", return)

    if (status == BluetoothConnectionStatus::BluetoothConnectionStatus_Connected) {
        setState(QLowEnergyController::ConnectedState);
        emit q->connected();
        return;
    }

    ComPtr<IBluetoothLEDevice3> device3;
    hr = mDevice.As(&device3);
    CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not cast device", return)
    ComPtr<IAsyncOperation<GattDeviceServicesResult *>> deviceServicesOp;
    hr = device3->GetGattServicesAsync(&deviceServicesOp);
    CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not obtain services", return)
    ComPtr<IGattDeviceServicesResult> deviceServicesResult;
    hr = QWinRTFunctions::await(deviceServicesOp, deviceServicesResult.GetAddressOf(),
                                QWinRTFunctions::ProcessMainThreadEvents, 5000);
    CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not await services operation", return)
    GattCommunicationStatus commStatus;
    hr = deviceServicesResult->get_Status(&commStatus);
    if (FAILED(hr) || commStatus != GattCommunicationStatus_Success) {
        qCWarning(QT_BT_WINRT()) << "Service operation failed";
        setError(QLowEnergyController::ConnectionError);
        setState(QLowEnergyController::UnconnectedState);
        return;
    }

    ComPtr<IVectorView <GattDeviceService *>> deviceServices;
    hr = deviceServicesResult->get_Services(&deviceServices);
    CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not obtain list of services", return)
    uint serviceCount;
    hr = deviceServices->get_Size(&serviceCount);
    CHECK_FOR_DEVICE_CONNECTION_ERROR(hr, "Could not obtain service count", return)

    // Windows automatically connects to the device as soon as a service value is read/written.
    // Thus we read one value in order to establish the connection.
    for (uint i = 0; i < serviceCount; ++i) {
        ComPtr<IGattDeviceService> service;
        hr = deviceServices->GetAt(i, &service);
        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain service");
        ComPtr<IGattDeviceService3> service3;
        hr = service.As(&service3);
        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not cast service");
        ComPtr<IAsyncOperation<GattCharacteristicsResult *>> characteristicsOp;
        hr = service3->GetCharacteristicsAsync(&characteristicsOp);
        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain characteristic");
        ComPtr<IGattCharacteristicsResult> characteristicsResult;
        hr = QWinRTFunctions::await(characteristicsOp, characteristicsResult.GetAddressOf(),
                                    QWinRTFunctions::ProcessMainThreadEvents, 5000);
        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not await characteristic operation");
        GattCommunicationStatus commStatus;
        hr = characteristicsResult->get_Status(&commStatus);
        if (FAILED(hr) || commStatus != GattCommunicationStatus_Success) {
            qCWarning(QT_BT_WINRT) << "Characteristic operation failed";
            continue;
        }
        ComPtr<IVectorView<GattCharacteristic *>> characteristics;
        hr = characteristicsResult->get_Characteristics(&characteristics);
        if (hr == E_ACCESSDENIED) {
            // Everything will work as expected up until this point if the manifest capabilties
            // for bluetooth LE are not set.
            qCWarning(QT_BT_WINRT) << "Could not obtain characteristic list. Please check your "
                                      "manifest capabilities";
            setState(QLowEnergyController::UnconnectedState);
            setError(QLowEnergyController::ConnectionError);
            return;
        }
        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain characteristic list");
        uint characteristicsCount;
        hr = characteristics->get_Size(&characteristicsCount);
        WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain characteristic list's size");
        for (uint j = 0; j < characteristicsCount; ++j) {
            ComPtr<IGattCharacteristic> characteristic;
            hr = characteristics->GetAt(j, &characteristic);
            WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain characteristic");
            ComPtr<IAsyncOperation<GattReadResult *>> op;
            GattCharacteristicProperties props;
            hr = characteristic->get_CharacteristicProperties(&props);
            WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain characteristic's properties");
            if (!(props & GattCharacteristicProperties_Read))
                continue;
            hr = characteristic->ReadValueWithCacheModeAsync(BluetoothCacheMode::BluetoothCacheMode_Uncached, &op);
            WARN_AND_CONTINUE_IF_FAILED(hr, "Could not read characteristic value");
            ComPtr<IGattReadResult> result;
            hr = QWinRTFunctions::await(op, result.GetAddressOf());
            WARN_AND_CONTINUE_IF_FAILED(hr, "Could await characteristic read");
            ComPtr<ABI::Windows::Storage::Streams::IBuffer> buffer;
            hr = result->get_Value(&buffer);
            WARN_AND_CONTINUE_IF_FAILED(hr, "Could not obtain characteristic value");
            if (!buffer) {
                qCDebug(QT_BT_WINRT) << "Problem reading value";
                continue;
            }
            return;
        }
    }

    qCWarning(QT_BT_WINRT) << "Could not obtain characteristic read result that triggers"
                            "device connection. Is the device reachable?";
    setError(QLowEnergyController::ConnectionError);
    setState(QLowEnergyController::UnconnectedState);
}

void QLowEnergyControllerPrivateWinRTNew::disconnectFromDevice()
{
    qCDebug(QT_BT_WINRT) << __FUNCTION__;
    Q_Q(QLowEnergyController);
    setState(QLowEnergyController::ClosingState);
    unregisterFromValueChanges();
    if (mDevice) {
        if (mStatusChangedToken.value) {
            mDevice->remove_ConnectionStatusChanged(mStatusChangedToken);
            mStatusChangedToken.value = 0;
        }
        mDevice = nullptr;
    }
    setState(QLowEnergyController::UnconnectedState);
    emit q->disconnected();
}

ComPtr<IGattDeviceService> QLowEnergyControllerPrivateWinRTNew::getNativeService(
        const QBluetoothUuid &serviceUuid)
{
    ComPtr<IGattDeviceService> deviceService;
    HRESULT hr;
    hr = mDevice->GetGattService(serviceUuid, &deviceService);
    if (FAILED(hr))
        qCDebug(QT_BT_WINRT) << "Could not obtain native service for Uuid" << serviceUuid;
    return deviceService;
}

ComPtr<IGattCharacteristic> QLowEnergyControllerPrivateWinRTNew::getNativeCharacteristic(
        const QBluetoothUuid &serviceUuid, const QBluetoothUuid &charUuid)
{
    ComPtr<IGattDeviceService> service = getNativeService(serviceUuid);
    if (!service)
        return nullptr;

    ComPtr<IVectorView<GattCharacteristic *>> characteristics;
    HRESULT hr = service->GetCharacteristics(charUuid, &characteristics);
    RETURN_IF_FAILED("Could not obtain native characteristics for service", return nullptr);
    ComPtr<IGattCharacteristic> characteristic;
    hr = characteristics->GetAt(0, &characteristic);
    RETURN_IF_FAILED("Could not obtain first characteristic for service", return nullptr);
    return characteristic;
}

void QLowEnergyControllerPrivateWinRTNew::registerForValueChanges(const QBluetoothUuid &serviceUuid,
                                                                  const QBluetoothUuid &charUuid)
{
    qCDebug(QT_BT_WINRT) << "Registering characteristic" << charUuid << "in service"
                         << serviceUuid << "for value changes";
    for (const ValueChangedEntry &entry : qAsConst(mValueChangedTokens)) {
        GUID guuid;
        HRESULT hr;
        hr = entry.characteristic->get_Uuid(&guuid);
        Q_ASSERT_SUCCEEDED(hr);
        if (QBluetoothUuid(guuid) == charUuid)
            return;
    }
    ComPtr<IGattCharacteristic> characteristic = getNativeCharacteristic(serviceUuid, charUuid);
    if (!characteristic) {
        qCDebug(QT_BT_WINRT).nospace() << "Could not obtain native characteristic " << charUuid
                             << " from service " << serviceUuid << ". Qt will not be able to signal"
                             << " changes for this characteristic.";
        return;
    }

    EventRegistrationToken token;
    HRESULT hr;
    hr = characteristic->add_ValueChanged(
                Callback<ValueChangedHandler>(
                    [this](IGattCharacteristic *characteristic, IGattValueChangedEventArgs *args) {
                    HRESULT hr;
                    quint16 handle;
                    hr = characteristic->get_AttributeHandle(&handle);
                    Q_ASSERT_SUCCEEDED(hr);
                    ComPtr<IBuffer> buffer;
                    hr = args->get_CharacteristicValue(&buffer);
                    Q_ASSERT_SUCCEEDED(hr);
                    characteristicChanged(handle, byteArrayFromBuffer(buffer));
                    return S_OK;
                }).Get(), &token);
    Q_ASSERT_SUCCEEDED(hr);
    mValueChangedTokens.append(ValueChangedEntry(characteristic, token));
    qCDebug(QT_BT_WINRT) << "Characteristic" << charUuid << "in service"
        << serviceUuid << "registered for value changes";
}

void QLowEnergyControllerPrivateWinRTNew::unregisterFromValueChanges()
{
    qCDebug(QT_BT_WINRT) << "Unregistering " << mValueChangedTokens.count() << " value change tokens";
    HRESULT hr;
    for (const ValueChangedEntry &entry : qAsConst(mValueChangedTokens)) {
        if (!entry.characteristic) {
            qCWarning(QT_BT_WINRT) << "Unregistering from value changes for characteristic failed."
                                   << "Characteristic has been deleted";
            continue;
        }
        hr = entry.characteristic->remove_ValueChanged(entry.token);
        if (FAILED(hr))
            qCWarning(QT_BT_WINRT) << "Unregistering from value changes for characteristic failed.";
    }
    mValueChangedTokens.clear();
}

void QLowEnergyControllerPrivateWinRTNew::obtainIncludedServices(
        QSharedPointer<QLowEnergyServicePrivate> servicePointer,
        ComPtr<IGattDeviceService> service)
{
    Q_Q(QLowEnergyController);
    ComPtr<IGattDeviceService2> service2;
    HRESULT hr = service.As(&service2);
    Q_ASSERT_SUCCEEDED(hr);
    ComPtr<IVectorView<GattDeviceService *>> includedServices;
    hr = service2->GetAllIncludedServices(&includedServices);
    // Some devices return ERROR_ACCESS_DISABLED_BY_POLICY
    if (FAILED(hr))
        return;

    uint count;
    hr = includedServices->get_Size(&count);
    Q_ASSERT_SUCCEEDED(hr);
    for (uint i = 0; i < count; ++i) {
        ComPtr<IGattDeviceService> includedService;
        hr = includedServices->GetAt(i, &includedService);
        Q_ASSERT_SUCCEEDED(hr);
        GUID guuid;
        hr = includedService->get_Uuid(&guuid);
        Q_ASSERT_SUCCEEDED(hr);
        const QBluetoothUuid includedUuid(guuid);
        QSharedPointer<QLowEnergyServicePrivate> includedPointer;
        if (serviceList.contains(includedUuid)) {
            includedPointer = serviceList.value(includedUuid);
        } else {
            QLowEnergyServicePrivate *priv = new QLowEnergyServicePrivate();
            priv->uuid = includedUuid;
            priv->setController(this);

            includedPointer = QSharedPointer<QLowEnergyServicePrivate>(priv);
            serviceList.insert(includedUuid, includedPointer);
        }
        includedPointer->type |= QLowEnergyService::IncludedService;
        servicePointer->includedServices.append(includedUuid);

        obtainIncludedServices(includedPointer, includedService);

        emit q->serviceDiscovered(includedUuid);
    }
}

void QLowEnergyControllerPrivateWinRTNew::discoverServices()
{
    Q_Q(QLowEnergyController);

    qCDebug(QT_BT_WINRT) << "Service discovery initiated";

    ComPtr<IBluetoothLEDevice3> device3;
    HRESULT hr = mDevice.As(&device3);
    Q_ASSERT_SUCCEEDED(hr);
    ComPtr<IAsyncOperation<GenericAttributeProfile::GattDeviceServicesResult *>> asyncResult;
    hr = device3->GetGattServicesAsync(&asyncResult);
    Q_ASSERT_SUCCEEDED(hr);
    hr = QEventDispatcherWinRT::runOnXamlThread( [asyncResult, q, this] () {
        HRESULT hr = asyncResult->put_Completed(
            Callback<IAsyncOperationCompletedHandler<GenericAttributeProfile::GattDeviceServicesResult *>>(
                    [this, q](IAsyncOperation<GenericAttributeProfile::GattDeviceServicesResult *> *, AsyncStatus status) {
                if (status != AsyncStatus::Completed) {
                    qCDebug(QT_BT_WINRT) << "Could not obtain services";
                    return S_OK;
                }
                ComPtr<IVectorView<GattDeviceService *>> deviceServices;
                HRESULT hr = mDevice->get_GattServices(&deviceServices);
                Q_ASSERT_SUCCEEDED(hr);
                uint serviceCount;
                hr = deviceServices->get_Size(&serviceCount);
                Q_ASSERT_SUCCEEDED(hr);
                for (uint i = 0; i < serviceCount; ++i) {
                    ComPtr<IGattDeviceService> deviceService;
                    hr = deviceServices->GetAt(i, &deviceService);
                    Q_ASSERT_SUCCEEDED(hr);
                    GUID guuid;
                    hr = deviceService->get_Uuid(&guuid);
                    Q_ASSERT_SUCCEEDED(hr);
                    const QBluetoothUuid service(guuid);

                    QSharedPointer<QLowEnergyServicePrivate> pointer;
                    if (serviceList.contains(service)) {
                        pointer = serviceList.value(service);
                    } else {
                        QLowEnergyServicePrivate *priv = new QLowEnergyServicePrivate();
                        priv->uuid = service;
                        priv->setController(this);

                        pointer = QSharedPointer<QLowEnergyServicePrivate>(priv);
                        serviceList.insert(service, pointer);
                    }
                    pointer->type |= QLowEnergyService::PrimaryService;

                    obtainIncludedServices(pointer, deviceService);

                    emit q->serviceDiscovered(service);
                }

                setState(QLowEnergyController::DiscoveredState);
                emit q->discoveryFinished();

                return S_OK;
            }).Get());
        Q_ASSERT_SUCCEEDED(hr);
        return hr;
    });
    Q_ASSERT_SUCCEEDED(hr);
}

void QLowEnergyControllerPrivateWinRTNew::discoverServiceDetails(const QBluetoothUuid &service)
{
    qCDebug(QT_BT_WINRT) << __FUNCTION__ << service;
    if (!serviceList.contains(service)) {
        qCWarning(QT_BT_WINRT) << "Discovery done of unknown service:"
            << service.toString();
        return;
    }

    ComPtr<IGattDeviceService> deviceService = getNativeService(service);
    if (!deviceService) {
        qCDebug(QT_BT_WINRT) << "Could not obtain native service for uuid " << service;
        return;
    }

    //update service data
    QSharedPointer<QLowEnergyServicePrivate> pointer = serviceList.value(service);

    pointer->setState(QLowEnergyService::DiscoveringServices);
    ComPtr<IGattDeviceService2> deviceService2;
    HRESULT hr = deviceService.As(&deviceService2);
    Q_ASSERT_SUCCEEDED(hr);
    ComPtr<IGattDeviceService3> deviceService3;
    hr = deviceService.As(&deviceService3);
    RETURN_IF_FAILED("Could not cast device service", return);
    ComPtr<IVectorView<GattDeviceService *>> deviceServices;
    hr = deviceService2->GetAllIncludedServices(&deviceServices);
    if (FAILED(hr)) { // ERROR_ACCESS_DISABLED_BY_POLICY
        qCDebug(QT_BT_WINRT) << "Could not obtain included services list for" << service;
        pointer->setError(QLowEnergyService::UnknownError);
        pointer->setState(QLowEnergyService::InvalidService);
        return;
    }
    uint serviceCount;
    hr = deviceServices->get_Size(&serviceCount);
    Q_ASSERT_SUCCEEDED(hr);
    for (uint i = 0; i < serviceCount; ++i) {
        ComPtr<IGattDeviceService> includedService;
        hr = deviceServices->GetAt(i, &includedService);
        Q_ASSERT_SUCCEEDED(hr);
        GUID guuid;
        hr = includedService->get_Uuid(&guuid);
        Q_ASSERT_SUCCEEDED(hr);

        const QBluetoothUuid service(guuid);
        if (service.isNull()) {
            qCDebug(QT_BT_WINRT) << "Could not find service";
            return;
        }

        pointer->includedServices.append(service);

        // update the type of the included service
        QSharedPointer<QLowEnergyServicePrivate> otherService = serviceList.value(service);
        if (!otherService.isNull())
            otherService->type |= QLowEnergyService::IncludedService;
    }

    QWinRTLowEnergyServiceHandlerNew *worker
            = new QWinRTLowEnergyServiceHandlerNew(service, deviceService3);
    QThread *thread = new QThread;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &QWinRTLowEnergyServiceHandlerNew::obtainCharList);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QWinRTLowEnergyServiceHandlerNew::errorOccured,
            this, &QLowEnergyControllerPrivateWinRTNew::handleServiceHandlerError);
    connect(worker, &QWinRTLowEnergyServiceHandlerNew::charListObtained,
            [this, thread](const QBluetoothUuid &service, QHash<QLowEnergyHandle,
            QLowEnergyServicePrivate::CharData> charList, QVector<QBluetoothUuid> indicateChars,
            QLowEnergyHandle startHandle, QLowEnergyHandle endHandle) {
        if (!serviceList.contains(service)) {
            qCWarning(QT_BT_WINRT) << "Discovery done of unknown service:"
                                   << service.toString();
            return;
        }

        QSharedPointer<QLowEnergyServicePrivate> pointer = serviceList.value(service);
        pointer->startHandle = startHandle;
        pointer->endHandle = endHandle;
        pointer->characteristicList = charList;

        HRESULT hr;
        hr = QEventDispatcherWinRT::runOnXamlThread([indicateChars, service, this]() {
            for (const QBluetoothUuid &indicateChar : qAsConst(indicateChars))
                registerForValueChanges(service, indicateChar);
            return S_OK;
        });
        Q_ASSERT_SUCCEEDED(hr);

        pointer->setState(QLowEnergyService::ServiceDiscovered);
        thread->exit(0);
    });
    thread->start();
}

void QLowEnergyControllerPrivateWinRTNew::startAdvertising(
        const QLowEnergyAdvertisingParameters &,
        const QLowEnergyAdvertisingData &,
        const QLowEnergyAdvertisingData &)
{
    setError(QLowEnergyController::AdvertisingError);
    Q_UNIMPLEMENTED();
}

void QLowEnergyControllerPrivateWinRTNew::stopAdvertising()
{
    Q_UNIMPLEMENTED();
}

void QLowEnergyControllerPrivateWinRTNew::requestConnectionUpdate(const QLowEnergyConnectionParameters &)
{
    Q_UNIMPLEMENTED();
}

void QLowEnergyControllerPrivateWinRTNew::readCharacteristic(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle)
{
    qCDebug(QT_BT_WINRT) << __FUNCTION__ << service << charHandle;
    Q_ASSERT(!service.isNull());
    if (role == QLowEnergyController::PeripheralRole) {
        service->setError(QLowEnergyService::CharacteristicReadError);
        Q_UNIMPLEMENTED();
        return;
    }

    if (!service->characteristicList.contains(charHandle)) {
        qCDebug(QT_BT_WINRT) << charHandle << "could not be found in service" << service->uuid;
        service->setError(QLowEnergyService::CharacteristicReadError);
        return;
    }

    HRESULT hr;
    hr = QEventDispatcherWinRT::runOnXamlThread([charHandle, service, this]() {
        const QLowEnergyServicePrivate::CharData charData = service->characteristicList.value(charHandle);
        if (!(charData.properties & QLowEnergyCharacteristic::Read))
            qCDebug(QT_BT_WINRT) << "Read flag is not set for characteristic" << charData.uuid;

        ComPtr<IGattCharacteristic> characteristic = getNativeCharacteristic(service->uuid, charData.uuid);
        if (!characteristic) {
            qCDebug(QT_BT_WINRT) << "Could not obtain native characteristic" << charData.uuid
                                 << "from service" << service->uuid;
            service->setError(QLowEnergyService::CharacteristicReadError);
            return S_OK;
        }
        ComPtr<IAsyncOperation<GattReadResult*>> readOp;
        HRESULT hr = characteristic->ReadValueWithCacheModeAsync(BluetoothCacheMode_Uncached, &readOp);
        Q_ASSERT_SUCCEEDED(hr);
        auto readCompletedLambda = [charData, charHandle, service]
                (IAsyncOperation<GattReadResult*> *op, AsyncStatus status)
        {
            if (status == AsyncStatus::Canceled || status == AsyncStatus::Error) {
                qCDebug(QT_BT_WINRT) << "Characteristic" << charHandle << "read operation failed.";
                service->setError(QLowEnergyService::CharacteristicReadError);
                return S_OK;
            }
            ComPtr<IGattReadResult> characteristicValue;
            HRESULT hr;
            hr = op->GetResults(&characteristicValue);
            if (FAILED(hr)) {
                qCDebug(QT_BT_WINRT) << "Could not obtain result for characteristic" << charHandle;
                service->setError(QLowEnergyService::CharacteristicReadError);
                return S_OK;
            }

            const QByteArray value = byteArrayFromGattResult(characteristicValue);
            QLowEnergyServicePrivate::CharData charData = service->characteristicList.value(charHandle);
            charData.value = value;
            service->characteristicList.insert(charHandle, charData);
            emit service->characteristicRead(QLowEnergyCharacteristic(service, charHandle), value);
            return S_OK;
        };
        hr = readOp->put_Completed(Callback<IAsyncOperationCompletedHandler<GattReadResult *>>(
                                       readCompletedLambda).Get());
        Q_ASSERT_SUCCEEDED(hr);
        return S_OK;
    });
    Q_ASSERT_SUCCEEDED(hr);
}

void QLowEnergyControllerPrivateWinRTNew::readDescriptor(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle,
        const QLowEnergyHandle descHandle)
{
    qCDebug(QT_BT_WINRT) << __FUNCTION__ << service << charHandle << descHandle;
    Q_ASSERT(!service.isNull());
    if (role == QLowEnergyController::PeripheralRole) {
        service->setError(QLowEnergyService::DescriptorReadError);
        Q_UNIMPLEMENTED();
        return;
    }

    if (!service->characteristicList.contains(charHandle)) {
        qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "in characteristic" << charHandle
                             << "cannot be found in service" << service->uuid;
        service->setError(QLowEnergyService::DescriptorReadError);
        return;
    }

    HRESULT hr;
    hr = QEventDispatcherWinRT::runOnXamlThread([charHandle, descHandle, service, this]() {
        QLowEnergyServicePrivate::CharData charData = service->characteristicList.value(charHandle);
        ComPtr<IGattCharacteristic> characteristic = getNativeCharacteristic(service->uuid, charData.uuid);
        if (!characteristic) {
            qCDebug(QT_BT_WINRT) << "Could not obtain native characteristic" << charData.uuid
                                 << "from service" << service->uuid;
            service->setError(QLowEnergyService::DescriptorReadError);
            return S_OK;
        }

        // Get native descriptor
        if (!charData.descriptorList.contains(descHandle))
            qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "cannot be found in characteristic" << charHandle;
        QLowEnergyServicePrivate::DescData descData = charData.descriptorList.value(descHandle);
        if (descData.uuid == QBluetoothUuid(QBluetoothUuid::ClientCharacteristicConfiguration)) {
            ComPtr<IAsyncOperation<ClientCharConfigDescriptorResult *>> readOp;
            HRESULT hr = characteristic->ReadClientCharacteristicConfigurationDescriptorAsync(&readOp);
            Q_ASSERT_SUCCEEDED(hr);
            auto readCompletedLambda = [&charData, charHandle, &descData, descHandle, service]
                    (IAsyncOperation<ClientCharConfigDescriptorResult *> *op, AsyncStatus status)
            {
                if (status == AsyncStatus::Canceled || status == AsyncStatus::Error) {
                    qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "read operation failed";
                    service->setError(QLowEnergyService::DescriptorReadError);
                    return S_OK;
                }
                ComPtr<IClientCharConfigDescriptorResult> iValue;
                HRESULT hr;
                hr = op->GetResults(&iValue);
                if (FAILED(hr)) {
                    qCDebug(QT_BT_WINRT) << "Could not obtain result for descriptor" << descHandle;
                    service->setError(QLowEnergyService::DescriptorReadError);
                    return S_OK;
                }
                GattClientCharacteristicConfigurationDescriptorValue value;
                hr = iValue->get_ClientCharacteristicConfigurationDescriptor(&value);
                if (FAILED(hr)) {
                    qCDebug(QT_BT_WINRT) << "Could not obtain value for descriptor" << descHandle;
                    service->setError(QLowEnergyService::DescriptorReadError);
                    return S_OK;
                }
                quint16 result = 0;
                bool correct = false;
                if (value & GattClientCharacteristicConfigurationDescriptorValue_Indicate) {
                    result |= QLowEnergyCharacteristic::Indicate;
                    correct = true;
                }
                if (value & GattClientCharacteristicConfigurationDescriptorValue_Notify) {
                    result |= QLowEnergyCharacteristic::Notify;
                    correct = true;
                }
                if (value == GattClientCharacteristicConfigurationDescriptorValue_None)
                    correct = true;
                if (!correct) {
                    qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle
                                         << "read operation failed. Obtained unexpected value.";
                    service->setError(QLowEnergyService::DescriptorReadError);
                    return S_OK;
                }
                descData.value = QByteArray(2, Qt::Uninitialized);
                qToLittleEndian(result, descData.value.data());
                charData.descriptorList.insert(descHandle, descData);
                emit service->descriptorRead(QLowEnergyDescriptor(service, charHandle, descHandle),
                    descData.value);
                return S_OK;
            };
            hr = readOp->put_Completed(
                        Callback<IAsyncOperationCompletedHandler<ClientCharConfigDescriptorResult *>>(
                            readCompletedLambda).Get());
            Q_ASSERT_SUCCEEDED(hr);
            return S_OK;
        } else {
            ComPtr<IVectorView<GattDescriptor *>> descriptors;
            HRESULT hr = characteristic->GetDescriptors(descData.uuid, &descriptors);
            Q_ASSERT_SUCCEEDED(hr);
            ComPtr<IGattDescriptor> descriptor;
            hr = descriptors->GetAt(0, &descriptor);
            Q_ASSERT_SUCCEEDED(hr);
            ComPtr<IAsyncOperation<GattReadResult*>> readOp;
            hr = descriptor->ReadValueWithCacheModeAsync(BluetoothCacheMode_Uncached, &readOp);
            Q_ASSERT_SUCCEEDED(hr);
            auto readCompletedLambda = [&charData, charHandle, &descData, descHandle, service]
                    (IAsyncOperation<GattReadResult*> *op, AsyncStatus status)
            {
                if (status == AsyncStatus::Canceled || status == AsyncStatus::Error) {
                    qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "read operation failed";
                    service->setError(QLowEnergyService::DescriptorReadError);
                    return S_OK;
                }
                ComPtr<IGattReadResult> descriptorValue;
                HRESULT hr;
                hr = op->GetResults(&descriptorValue);
                if (FAILED(hr)) {
                    qCDebug(QT_BT_WINRT) << "Could not obtain result for descriptor" << descHandle;
                    service->setError(QLowEnergyService::DescriptorReadError);
                    return S_OK;
                }
                if (descData.uuid == QBluetoothUuid::CharacteristicUserDescription)
                    descData.value = byteArrayFromGattResult(descriptorValue, true);
                else
                    descData.value = byteArrayFromGattResult(descriptorValue);
                charData.descriptorList.insert(descHandle, descData);
                emit service->descriptorRead(QLowEnergyDescriptor(service, charHandle, descHandle),
                    descData.value);
                return S_OK;
            };
            hr = readOp->put_Completed(Callback<IAsyncOperationCompletedHandler<GattReadResult *>>(
                                           readCompletedLambda).Get());
            Q_ASSERT_SUCCEEDED(hr);
            return S_OK;
        }
    });
    Q_ASSERT_SUCCEEDED(hr);
}

void QLowEnergyControllerPrivateWinRTNew::writeCharacteristic(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle,
        const QByteArray &newValue,
        QLowEnergyService::WriteMode mode)
{
    qCDebug(QT_BT_WINRT) << __FUNCTION__ << service << charHandle << newValue << mode;
    Q_ASSERT(!service.isNull());
    if (role == QLowEnergyController::PeripheralRole) {
        service->setError(QLowEnergyService::CharacteristicWriteError);
        Q_UNIMPLEMENTED();
        return;
    }
    if (!service->characteristicList.contains(charHandle)) {
        qCDebug(QT_BT_WINRT) << "Characteristic" << charHandle << "cannot be found in service"
                             << service->uuid;
        service->setError(QLowEnergyService::CharacteristicWriteError);
        return;
    }

    QLowEnergyServicePrivate::CharData charData = service->characteristicList.value(charHandle);
    const bool writeWithResponse = mode == QLowEnergyService::WriteWithResponse;
    if (!(charData.properties & (writeWithResponse ? QLowEnergyCharacteristic::Write
             : QLowEnergyCharacteristic::WriteNoResponse)))
        qCDebug(QT_BT_WINRT) << "Write flag is not set for characteristic" << charHandle;

    HRESULT hr;
    hr = QEventDispatcherWinRT::runOnXamlThread([charData, charHandle, this, service, newValue,
                                                writeWithResponse]() {
        ComPtr<IGattCharacteristic> characteristic = getNativeCharacteristic(service->uuid,
                                                                             charData.uuid);
        if (!characteristic) {
            qCDebug(QT_BT_WINRT) << "Could not obtain native characteristic" << charData.uuid
                                 << "from service" << service->uuid;
            service->setError(QLowEnergyService::CharacteristicWriteError);
            return S_OK;
        }
        ComPtr<ABI::Windows::Storage::Streams::IBufferFactory> bufferFactory;
        HRESULT hr = GetActivationFactory(
                    HStringReference(RuntimeClass_Windows_Storage_Streams_Buffer).Get(),
                    &bufferFactory);
        Q_ASSERT_SUCCEEDED(hr);
        ComPtr<ABI::Windows::Storage::Streams::IBuffer> buffer;
        const quint32 length = quint32(newValue.length());
        hr = bufferFactory->Create(length, &buffer);
        Q_ASSERT_SUCCEEDED(hr);
        hr = buffer->put_Length(length);
        Q_ASSERT_SUCCEEDED(hr);
        ComPtr<Windows::Storage::Streams::IBufferByteAccess> byteAccess;
        hr = buffer.As(&byteAccess);
        Q_ASSERT_SUCCEEDED(hr);
        byte *bytes;
        hr = byteAccess->Buffer(&bytes);
        Q_ASSERT_SUCCEEDED(hr);
        memcpy(bytes, newValue, length);
        ComPtr<IAsyncOperation<GattCommunicationStatus>> writeOp;
        GattWriteOption option = writeWithResponse ? GattWriteOption_WriteWithResponse
                                                   : GattWriteOption_WriteWithoutResponse;
        hr = characteristic->WriteValueWithOptionAsync(buffer.Get(), option, &writeOp);
        Q_ASSERT_SUCCEEDED(hr);
        auto writeCompletedLambda =[charData, charHandle, newValue, service, writeWithResponse, this]
                (IAsyncOperation<GattCommunicationStatus> *op, AsyncStatus status)
        {
            if (status == AsyncStatus::Canceled || status == AsyncStatus::Error) {
                qCDebug(QT_BT_WINRT) << "Characteristic" << charHandle << "write operation failed";
                service->setError(QLowEnergyService::CharacteristicWriteError);
                return S_OK;
            }
            GattCommunicationStatus result;
            HRESULT hr;
            hr = op->GetResults(&result);
            if (hr == E_BLUETOOTH_ATT_INVALID_ATTRIBUTE_VALUE_LENGTH) {
                qCDebug(QT_BT_WINRT) << "Characteristic" << charHandle
                                     << "write operation was tried with invalid value length";
                service->setError(QLowEnergyService::CharacteristicWriteError);
                return S_OK;
            }
            Q_ASSERT_SUCCEEDED(hr);
            if (result != GattCommunicationStatus_Success) {
                qCDebug(QT_BT_WINRT) << "Characteristic" << charHandle << "write operation failed";
                service->setError(QLowEnergyService::CharacteristicWriteError);
                return S_OK;
            }
            // only update cache when property is readable. Otherwise it remains
            // empty.
            if (charData.properties & QLowEnergyCharacteristic::Read)
                updateValueOfCharacteristic(charHandle, newValue, false);
            if (writeWithResponse)
                emit service->characteristicWritten(QLowEnergyCharacteristic(service, charHandle),
                                                    newValue);
            return S_OK;
        };
        hr = writeOp->put_Completed(
                    Callback<IAsyncOperationCompletedHandler<GattCommunicationStatus>>(
                        writeCompletedLambda).Get());
        Q_ASSERT_SUCCEEDED(hr);
        return S_OK;
    });
    Q_ASSERT_SUCCEEDED(hr);
}

void QLowEnergyControllerPrivateWinRTNew::writeDescriptor(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle,
        const QLowEnergyHandle descHandle,
        const QByteArray &newValue)
{
    qCDebug(QT_BT_WINRT) << __FUNCTION__ << service << charHandle << descHandle << newValue;
    Q_ASSERT(!service.isNull());
    if (role == QLowEnergyController::PeripheralRole) {
        service->setError(QLowEnergyService::DescriptorWriteError);
        Q_UNIMPLEMENTED();
        return;
    }

    if (!service->characteristicList.contains(charHandle)) {
        qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "in characteristic" << charHandle
                             << "could not be found in service" << service->uuid;
        service->setError(QLowEnergyService::DescriptorWriteError);
        return;
    }

    HRESULT hr;
    hr = QEventDispatcherWinRT::runOnXamlThread([charHandle, descHandle, this, service, newValue]() {
        const QLowEnergyServicePrivate::CharData charData = service->characteristicList.value(charHandle);
        ComPtr<IGattCharacteristic> characteristic = getNativeCharacteristic(service->uuid, charData.uuid);
        if (!characteristic) {
            qCDebug(QT_BT_WINRT) << "Could not obtain native characteristic" << charData.uuid
                                 << "from service" << service->uuid;
            service->setError(QLowEnergyService::DescriptorWriteError);
            return S_OK;
        }

        // Get native descriptor
        if (!charData.descriptorList.contains(descHandle))
            qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "could not be found in Characteristic"
                                 << charHandle;

        QLowEnergyServicePrivate::DescData descData = charData.descriptorList.value(descHandle);
        if (descData.uuid == QBluetoothUuid(QBluetoothUuid::ClientCharacteristicConfiguration)) {
            GattClientCharacteristicConfigurationDescriptorValue value;
            quint16 intValue = qFromLittleEndian<quint16>(newValue);
            if (intValue & GattClientCharacteristicConfigurationDescriptorValue_Indicate
                    && intValue & GattClientCharacteristicConfigurationDescriptorValue_Notify) {
                qCWarning(QT_BT_WINRT) << "Setting both Indicate and Notify is not supported on WinRT";
                value = GattClientCharacteristicConfigurationDescriptorValue(
                        (GattClientCharacteristicConfigurationDescriptorValue_Indicate
                         | GattClientCharacteristicConfigurationDescriptorValue_Notify));
            } else if (intValue & GattClientCharacteristicConfigurationDescriptorValue_Indicate) {
                value = GattClientCharacteristicConfigurationDescriptorValue_Indicate;
            } else if (intValue & GattClientCharacteristicConfigurationDescriptorValue_Notify) {
                value = GattClientCharacteristicConfigurationDescriptorValue_Notify;
            } else if (intValue == 0) {
                value = GattClientCharacteristicConfigurationDescriptorValue_None;
            } else {
                qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle
                                     << "write operation failed: Invalid value";
                service->setError(QLowEnergyService::DescriptorWriteError);
                return S_OK;
            }
            ComPtr<IAsyncOperation<enum GattCommunicationStatus>> writeOp;
            HRESULT hr = characteristic->WriteClientCharacteristicConfigurationDescriptorAsync(value, &writeOp);
            Q_ASSERT_SUCCEEDED(hr);
            auto writeCompletedLambda = [charHandle, descHandle, newValue, service, this]
                    (IAsyncOperation<GattCommunicationStatus> *op, AsyncStatus status)
            {
                if (status == AsyncStatus::Canceled || status == AsyncStatus::Error) {
                    qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "write operation failed";
                    service->setError(QLowEnergyService::DescriptorWriteError);
                    return S_OK;
                }
                GattCommunicationStatus result;
                HRESULT hr;
                hr = op->GetResults(&result);
                if (FAILED(hr)) {
                    qCDebug(QT_BT_WINRT) << "Could not obtain result for descriptor" << descHandle;
                    service->setError(QLowEnergyService::DescriptorWriteError);
                    return S_OK;
                }
                if (result != GattCommunicationStatus_Success) {
                    qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "write operation failed";
                    service->setError(QLowEnergyService::DescriptorWriteError);
                    return S_OK;
                }
                updateValueOfDescriptor(charHandle, descHandle, newValue, false);
                emit service->descriptorWritten(QLowEnergyDescriptor(service, charHandle, descHandle),
                                                newValue);
                return S_OK;
            };
            hr = writeOp->put_Completed(
                        Callback<IAsyncOperationCompletedHandler<GattCommunicationStatus >>(
                            writeCompletedLambda).Get());
            Q_ASSERT_SUCCEEDED(hr);
        } else {
            ComPtr<IVectorView<GattDescriptor *>> descriptors;
            HRESULT hr = characteristic->GetDescriptors(descData.uuid, &descriptors);
            Q_ASSERT_SUCCEEDED(hr);
            ComPtr<IGattDescriptor> descriptor;
            hr = descriptors->GetAt(0, &descriptor);
            Q_ASSERT_SUCCEEDED(hr);
            ComPtr<ABI::Windows::Storage::Streams::IBufferFactory> bufferFactory;
            hr = GetActivationFactory(
                        HStringReference(RuntimeClass_Windows_Storage_Streams_Buffer).Get(),
                        &bufferFactory);
            Q_ASSERT_SUCCEEDED(hr);
            ComPtr<ABI::Windows::Storage::Streams::IBuffer> buffer;
            const quint32 length = quint32(newValue.length());
            hr = bufferFactory->Create(length, &buffer);
            Q_ASSERT_SUCCEEDED(hr);
            hr = buffer->put_Length(length);
            Q_ASSERT_SUCCEEDED(hr);
            ComPtr<Windows::Storage::Streams::IBufferByteAccess> byteAccess;
            hr = buffer.As(&byteAccess);
            Q_ASSERT_SUCCEEDED(hr);
            byte *bytes;
            hr = byteAccess->Buffer(&bytes);
            Q_ASSERT_SUCCEEDED(hr);
            memcpy(bytes, newValue, length);
            ComPtr<IAsyncOperation<GattCommunicationStatus>> writeOp;
            hr = descriptor->WriteValueAsync(buffer.Get(), &writeOp);
            Q_ASSERT_SUCCEEDED(hr);
            auto writeCompletedLambda = [charHandle, descHandle, newValue, service, this]
                    (IAsyncOperation<GattCommunicationStatus> *op, AsyncStatus status)
            {
                if (status == AsyncStatus::Canceled || status == AsyncStatus::Error) {
                    qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "write operation failed";
                    service->setError(QLowEnergyService::DescriptorWriteError);
                    return S_OK;
                }
                GattCommunicationStatus result;
                HRESULT hr;
                hr = op->GetResults(&result);
                if (FAILED(hr)) {
                    qCDebug(QT_BT_WINRT) << "Could not obtain result for descriptor" << descHandle;
                    service->setError(QLowEnergyService::DescriptorWriteError);
                    return S_OK;
                }
                if (result != GattCommunicationStatus_Success) {
                    qCDebug(QT_BT_WINRT) << "Descriptor" << descHandle << "write operation failed";
                    service->setError(QLowEnergyService::DescriptorWriteError);
                    return S_OK;
                }
                updateValueOfDescriptor(charHandle, descHandle, newValue, false);
                emit service->descriptorWritten(QLowEnergyDescriptor(service, charHandle, descHandle),
                                                newValue);
                return S_OK;
            };
            hr = writeOp->put_Completed(
                        Callback<IAsyncOperationCompletedHandler<GattCommunicationStatus>>(
                            writeCompletedLambda).Get());
            Q_ASSERT_SUCCEEDED(hr);
            return S_OK;
        }
        return S_OK;
    });
    Q_ASSERT_SUCCEEDED(hr);
}


void QLowEnergyControllerPrivateWinRTNew::addToGenericAttributeList(const QLowEnergyServiceData &,
                                                                    QLowEnergyHandle)
{
    Q_UNIMPLEMENTED();
}

void QLowEnergyControllerPrivateWinRTNew::characteristicChanged(
        quint16 charHandle, const QByteArray &data)
{
    QSharedPointer<QLowEnergyServicePrivate> service =
            serviceForHandle(charHandle);
    if (service.isNull())
        return;

    qCDebug(QT_BT_WINRT) << "Characteristic change notification" << service->uuid
                           << charHandle << data.toHex();

    QLowEnergyCharacteristic characteristic = characteristicForHandle(charHandle);
    if (!characteristic.isValid()) {
        qCWarning(QT_BT_WINRT) << "characteristicChanged: Cannot find characteristic";
        return;
    }

    // only update cache when property is readable. Otherwise it remains
    // empty.
    if (characteristic.properties() & QLowEnergyCharacteristic::Read)
        updateValueOfCharacteristic(characteristic.attributeHandle(),
                                data, false);
    emit service->characteristicChanged(characteristic, data);
}

void QLowEnergyControllerPrivateWinRTNew::handleServiceHandlerError(const QString &error)
{
    if (state != QLowEnergyController::DiscoveringState)
        return;

    qCWarning(QT_BT_WINRT) << "Error while discovering services:" << error;
    setState(QLowEnergyController::UnconnectedState);
    setError(QLowEnergyController::ConnectionError);
}

QT_END_NAMESPACE

#include "qlowenergycontroller_winrt_new.moc"
