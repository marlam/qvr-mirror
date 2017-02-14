/*
 * Copyright (C) 2016, 2017 Computer Graphics Group, University of Siegen
 * Written by Martin Lambers <martin.lambers@uni-siegen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <QtMath>

#include "manager.hpp"
#include "device.hpp"
#include "logging.hpp"
#include "internalglobals.hpp"

#ifdef HAVE_QGAMEPAD
# include <QGamepad>
#endif
#ifdef HAVE_VRPN
# include <vrpn_Tracker.h>
# include <vrpn_Analog.h>
# include <vrpn_Button.h>
#endif
#ifdef HAVE_OSVR
# include <osvr/ClientKit/InterfaceStateC.h>
# include <osvr/ClientKit/InterfaceC.h>
#endif


struct QVRDeviceInternals {
    // Last known position and orientation, with timestamp.
    // These are used to calculate velocity and angular velocity.
    // If the timestamp is -1 then we do not have known values yet.
    qint64 currentTimestamp;
    qint64 lastTimestamp;
    QVector3D lastPosition;
    QQuaternion lastOrientation;
#ifdef HAVE_QGAMEPAD
    QGamepad* buttonsGamepad; // these pointers might actually...
    QGamepad* analogsGamepad; // ...point to the same gamepad object!
#endif
#ifdef HAVE_VRPN
    QVector3D* vrpnPositionPtr; // pointer to _position
    QQuaternion* vrpnOrientationPtr; // pointer to _orientation
    QVector3D* vrpnVelocityPtr; // pointer to _velocity
    QVector3D* vrpnAngularVelocityPtr; // pointer to _angularVelocity;
    bool vrpnHaveVelocity;
    QVector<bool>* vrpnButtonsPtr; // pointer to _buttons
    QVector<float>* vrpnAnalogsPtr; // pointer to _analogs
    vrpn_Tracker_Remote* vrpnTrackerRemote;
    vrpn_Button_Remote* vrpnButtonRemote;
    QVector<int> vrpnButtons;
    vrpn_Analog_Remote* vrpnAnalogRemote;
    QVector<float> vrpnAnalogs;
#endif
#ifdef HAVE_OCULUS
    int oculusTrackedEntity; // -1 = none, 0 = center/head, 1 = left eye, 2 = right eye, 3 = left controller, 4 = right controller
    int oculusButtonsEntity; // -1 = none, 0 = xbox, 1 = touch left, 2 = touch right
    int oculusAnalogsEntity; // -1 = none, 0 = xbox, 1 = touch left, 2 = touch right
#endif
#ifdef HAVE_OPENVR
    int openVrTrackedEntity; // -1 = none, 0 = center/head, 1 = left eye, 2 = right eye, 3 = controller0, 4 = controller1
    int openVrButtonsEntity; // -1 = none, 0 = controller0, 1 = controller1
    int openVrAnalogsEntity; // -1 = none, 0 = controller0, 1 = controller1
#endif
#ifdef HAVE_OSVR
    int osvrTrackedEye; // -1 = none, 0 = center, 1 = left, 2 = right
    OSVR_ClientInterface osvrTrackingInterface;
    QVector<OSVR_ClientInterface> osvrButtonsInterfaces;
    QVector<OSVR_ClientInterface> osvrAnalogsInterfaces;
#endif
};

static QVector3D QVRAngularVelocityFromDiffQuaternion(const QQuaternion& q, double seconds)
{
    QVector3D axis;
    float angle;
    q.getAxisAndAngle(&axis, &angle);
    return axis * qDegreesToRadians(angle) / seconds;
}

#ifdef HAVE_VRPN
void QVRVrpnTrackerChangeHandler(void* userdata, const vrpn_TRACKERCB info)
{
    struct QVRDeviceInternals* d = reinterpret_cast<struct QVRDeviceInternals*>(userdata);
    *(d->vrpnPositionPtr) = QVector3D(info.pos[0], info.pos[1], info.pos[2]);
    *(d->vrpnOrientationPtr) = QQuaternion(info.quat[3], info.quat[0], info.quat[1], info.quat[2]);
}
void QVRVrpnTrackerVelocityChangeHandler(void* userdata, const vrpn_TRACKERVELCB info)
{
    struct QVRDeviceInternals* d = reinterpret_cast<struct QVRDeviceInternals*>(userdata);
    *(d->vrpnVelocityPtr) = QVector3D(info.vel[0], info.vel[1], info.vel[2]);
    *(d->vrpnAngularVelocityPtr) = QVRAngularVelocityFromDiffQuaternion(
            QQuaternion(info.vel_quat[3], info.vel_quat[0], info.vel_quat[1], info.vel_quat[2]),
            info.vel_quat_dt);
    d->vrpnHaveVelocity = true;
}
void QVRVrpnButtonChangeHandler(void* userdata, const vrpn_BUTTONCB info)
{
    struct QVRDeviceInternals* d = reinterpret_cast<struct QVRDeviceInternals*>(userdata);
    for (int i = 0; i < d->vrpnButtonsPtr->length(); i++) {
        if (info.button == d->vrpnButtons[i])
            (*(d->vrpnButtonsPtr))[i] = info.state;
    }
}
void QVRVrpnAnalogChangeHandler(void* userdata, const vrpn_ANALOGCB info)
{
    struct QVRDeviceInternals* d = reinterpret_cast<struct QVRDeviceInternals*>(userdata);
    for (int i = 0; i < d->vrpnAnalogsPtr->length(); i++) {
        int j = d->vrpnAnalogs[i];
        if (j >= 0 && j < info.num_channel)
            (*(d->vrpnAnalogsPtr))[i] = info.channel[j];
    }
}
#endif


QVRDevice::QVRDevice() :
    _index(-1), _internals(NULL)
{
}

QVRDevice::QVRDevice(int deviceIndex) :
    _index(deviceIndex)
{
    _internals = new struct QVRDeviceInternals;
    _internals->currentTimestamp = -1;
#ifdef HAVE_QGAMEPAD
    _internals->buttonsGamepad = NULL;
    _internals->analogsGamepad = NULL;
#endif
#ifdef HAVE_VRPN
    _internals->vrpnPositionPtr = &_position;
    _internals->vrpnOrientationPtr = &_orientation;
    _internals->vrpnVelocityPtr = &_velocity;
    _internals->vrpnAngularVelocityPtr = &_angularVelocity;
    _internals->vrpnHaveVelocity = false;
    _internals->vrpnButtonsPtr = &_buttons;
    _internals->vrpnAnalogsPtr = &_analogs;
    _internals->vrpnTrackerRemote = NULL;
    _internals->vrpnAnalogRemote = NULL;
    _internals->vrpnButtonRemote = NULL;
#endif
#ifdef HAVE_OCULUS
    _internals->oculusTrackedEntity = -1;
    _internals->oculusButtonsEntity = -1;
    _internals->oculusAnalogsEntity = -1;
#endif
#ifdef HAVE_OPENVR
    _internals->openVrTrackedEntity = -1;
    _internals->openVrButtonsEntity = -1;
    _internals->openVrAnalogsEntity = -1;
#endif
#ifdef HAVE_OSVR
    _internals->osvrTrackedEye = -1;
    _internals->osvrTrackingInterface = NULL;
#endif

    switch (config().trackingType()) {
    case QVR_Device_Tracking_None:
        break;
    case QVR_Device_Tracking_Static:
        {
            QStringList args = config().trackingParameters().split(' ', QString::SkipEmptyParts);
            if (args.length() == 3 || args.length() == 6)
                _position = QVector3D(args[0].toFloat(), args[1].toFloat(), args[2].toFloat());
            if (args.length() == 6)
                _orientation = QQuaternion::fromEulerAngles(args[3].toFloat(), args[4].toFloat(), args[5].toFloat());
        }
        break;
    case QVR_Device_Tracking_VRPN:
#ifdef HAVE_VRPN
        if (QVRManager::processIndex() == config().processIndex()) {
            QStringList args = config().trackingParameters().split(' ', QString::SkipEmptyParts);
            QString name = (args.length() >= 1 ? args[0] : config().trackingParameters());
            int sensor = (args.length() >= 2 ? args[1].toInt() : vrpn_ALL_SENSORS);
            _internals->vrpnTrackerRemote = new vrpn_Tracker_Remote(qPrintable(name));
            vrpn_System_TextPrinter.set_ostream_to_use(stderr);
            _internals->vrpnTrackerRemote->register_change_handler(_internals, QVRVrpnTrackerChangeHandler, sensor);
            _internals->vrpnTrackerRemote->register_change_handler(_internals, QVRVrpnTrackerVelocityChangeHandler, sensor);
        }
#endif
        break;
    case QVR_Device_Tracking_Oculus:
#ifdef HAVE_OCULUS
        if (QVRManager::processIndex() == config().processIndex()) {
            QString arg = config().trackingParameters().trimmed();
            if (arg == "head")
                _internals->oculusTrackedEntity = 0;
            else if (arg == "eye-left")
                _internals->oculusTrackedEntity = 1;
            else if (arg == "eye-right")
                _internals->oculusTrackedEntity = 2;
            else if (arg == "controller-left")
                _internals->oculusTrackedEntity = 3;
            else if (arg == "controller-right")
                _internals->oculusTrackedEntity = 4;
            else
                QVR_WARNING("device %s: invalid Oculus tracking parameter", qPrintable(id()));
        }
#endif
        break;
    case QVR_Device_Tracking_OpenVR:
#ifdef HAVE_OPENVR
        if (QVRManager::processIndex() == config().processIndex()) {
            QString arg = config().trackingParameters().trimmed();
            if (arg == "head")
                _internals->openVrTrackedEntity = 0;
            else if (arg == "eye-left")
                _internals->openVrTrackedEntity = 1;
            else if (arg == "eye-right")
                _internals->openVrTrackedEntity = 2;
            else if (arg == "controller-0")
                _internals->openVrTrackedEntity = 3;
            else if (arg == "controller-1")
                _internals->openVrTrackedEntity = 4;
            else
                QVR_WARNING("device %s: invalid OpenVR tracking parameter", qPrintable(id()));
        }
#endif
        break;
    case QVR_Device_Tracking_OSVR:
#ifdef HAVE_OSVR
        if (QVRManager::processIndex() == config().processIndex()) {
            Q_ASSERT(QVROsvrClientContext);
            const QString& osvrPath = config().trackingParameters();
            if (osvrPath == "eye-center") {
                _internals->osvrTrackedEye = 0;
            } else if (osvrPath == "eye-left") {
                _internals->osvrTrackedEye = 1;
            } else if (osvrPath == "eye-right") {
                _internals->osvrTrackedEye = 2;
            } else {
                osvrClientGetInterface(QVROsvrClientContext, qPrintable(osvrPath), &_internals->osvrTrackingInterface);
                if (!_internals->osvrTrackingInterface) {
                    QVR_WARNING("device %s: OSVR interface path %s does not exist",
                            qPrintable(id()), qPrintable(osvrPath));
                }
            }
        }
#endif
        break;
    }

    switch (config().buttonsType()) {
    case QVR_Device_Buttons_None:
        break;
    case QVR_Device_Buttons_Static:
        {
            QStringList args = config().buttonsParameters().split(' ', QString::SkipEmptyParts);
            _buttons.resize(args.length());
            for (int i = 0; i < _buttons.length(); i++)
                _buttons[i] = args[i].toInt();
        }
        break;
    case QVR_Device_Buttons_Gamepad:
#ifdef HAVE_QGAMEPAD
        {
            if (QVRManager::processIndex() == config().processIndex()) {
                QString arg = config().buttonsParameters().trimmed();
                int padId = arg.toInt();
                if (padId < 0 || padId >= QVRGamepads.size()) {
                    QVR_WARNING("device %s: buttons gamepad %d is not connected", qPrintable(id()), padId);
                } else {
                    int padDeviceId = QVRGamepads[padId];
                    _internals->buttonsGamepad = new QGamepad(padDeviceId);
                    QVR_DEBUG("device %s uses gamepad %d for buttons", qPrintable(id()), padId);
                }
            }
            _buttons.resize(18);
            for (int i = 0; i < _buttons.length(); i++)
                _buttons[i] = false;
        }
#endif
        break;
    case QVR_Device_Buttons_VRPN:
#ifdef HAVE_VRPN
        {
            QStringList args = config().buttonsParameters().split(' ', QString::SkipEmptyParts);
            QString name = (args.length() >= 1 ? args[0] : config().buttonsParameters());
            if (args.length() > 1) {
                _buttons.resize(args.length() - 1);
                _internals->vrpnButtons.resize(_buttons.length());
                for (int i = 0; i < _buttons.length(); i++)
                    _internals->vrpnButtons[i] = args[i + 1].toInt();
            } else {
                _buttons.resize(32);
                _internals->vrpnButtons.resize(_buttons.length());
                for (int i = 0; i < _buttons.length(); i++)
                    _internals->vrpnButtons[i] = i;
            }
            for (int i = 0; i < _buttons.length(); i++)
                _buttons[i] = false;
            if (QVRManager::processIndex() == config().processIndex()) {
                _internals->vrpnButtonRemote = new vrpn_Button_Remote(qPrintable(name));
                vrpn_System_TextPrinter.set_ostream_to_use(stderr);
                _internals->vrpnButtonRemote->register_change_handler(_internals, QVRVrpnButtonChangeHandler);
            }
        }
#endif
        break;
    case QVR_Device_Buttons_Oculus:
#ifdef HAVE_OCULUS
        {
            QString arg = config().buttonsParameters().trimmed();
            if (arg == "xbox") {
                _buttons.resize(12);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROculus);
                    _internals->oculusButtonsEntity = 0;
                }
            } else if (arg == "controller-left") {
                _buttons.resize(8);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROculus);
                    _internals->oculusButtonsEntity = 1;
                }
            } else if (arg == "controller-right") {
                _buttons.resize(8);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROculus);
                    _internals->oculusButtonsEntity = 2;
                }
            } else {
                QVR_WARNING("device %s: invalid Oculus buttons parameter", qPrintable(id()));
            }
        }
#endif
        break;
    case QVR_Device_Buttons_OpenVR:
#ifdef HAVE_OPENVR
        {
            QString arg = config().buttonsParameters().trimmed();
            if (arg == "controller-0") {
                _buttons.resize(6);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROpenVRSystem);
                    _internals->openVrButtonsEntity = 0;
                }
            } else if (arg == "controller-1") {
                _buttons.resize(6);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROpenVRSystem);
                    _internals->openVrButtonsEntity = 1;
                }
            } else {
                QVR_WARNING("device %s: invalid OpenVR buttons parameter", qPrintable(id()));
            }
        }
#endif
        break;
    case QVR_Device_Buttons_OSVR:
#ifdef HAVE_OSVR
        {
            QStringList args = config().buttonsParameters().split(' ', QString::SkipEmptyParts);
            _buttons.resize(args.length());
            if (QVRManager::processIndex() == config().processIndex()) {
                Q_ASSERT(QVROsvrClientContext);
                for (int i = 0; i < _buttons.length(); i++) {
                    const QString& osvrPath = args[i];
                    OSVR_ClientInterface osvrInterface;
                    osvrClientGetInterface(QVROsvrClientContext, qPrintable(osvrPath), &osvrInterface);
                    if (!osvrInterface) {
                        QVR_WARNING("device %s: OSVR interface path %s does not exist",
                                qPrintable(id()), qPrintable(osvrPath));
                    }
                    _internals->osvrButtonsInterfaces.append(osvrInterface);
                }
            }
        }
#endif
        break;
    }

    switch (config().analogsType()) {
    case QVR_Device_Analogs_None:
        break;
    case QVR_Device_Analogs_Static:
        {
            QStringList args = config().analogsParameters().split(' ', QString::SkipEmptyParts);
            _analogs.resize(args.length());
            for (int i = 0; i < _analogs.length(); i++)
                _analogs[i] = args[i].toFloat();
        }
        break;
    case QVR_Device_Analogs_Gamepad:
#ifdef HAVE_QGAMEPAD
        {
            if (QVRManager::processIndex() == config().processIndex()) {
                QString arg = config().analogsParameters().trimmed();
                int padId = arg.toInt();
                if (padId < 0 || padId >= QVRGamepads.size()) {
                    QVR_WARNING("device %s: analogs gamepad %d is not connected", qPrintable(id()), padId);
                } else {
                    int padDeviceId = QVRGamepads[padId];
                    if (_internals->buttonsGamepad && padDeviceId == _internals->buttonsGamepad->deviceId())
                        _internals->analogsGamepad = _internals->buttonsGamepad;
                    else
                        _internals->analogsGamepad = new QGamepad(padDeviceId);
                    QVR_DEBUG("device %s uses gamepad %d for analogs", qPrintable(id()), padId);
                }
            }
            _analogs.resize(4);
            for (int i = 0; i < _analogs.length(); i++)
                _analogs[i] = 0.0f;
        }
#endif
        break;
    case QVR_Device_Analogs_VRPN:
#ifdef HAVE_VRPN
        {
            QStringList args = config().analogsParameters().split(' ', QString::SkipEmptyParts);
            QString name = (args.length() >= 1 ? args[0] : config().analogsParameters());
            if (args.length() > 1) {
                _analogs.resize(args.length() - 1);
                _internals->vrpnAnalogs.resize(_analogs.length());
                for (int i = 0; i < _analogs.length(); i++)
                    _internals->vrpnAnalogs[i] = args[i + 1].toInt();
            } else {
                _analogs.resize(8);
                _internals->vrpnAnalogs.resize(_analogs.length());
                for (int i = 0; i < _analogs.length(); i++)
                    _internals->vrpnAnalogs[i] = i;
            }
            for (int i = 0; i < _analogs.length(); i++)
                _analogs[i] = 0.0f;
            if (QVRManager::processIndex() == config().processIndex()) {
                _internals->vrpnAnalogRemote = new vrpn_Analog_Remote(qPrintable(name));
                vrpn_System_TextPrinter.set_ostream_to_use(stderr);
                _internals->vrpnAnalogRemote->register_change_handler(_internals, QVRVrpnAnalogChangeHandler);
            }
        }
#endif
        break;
    case QVR_Device_Analogs_Oculus:
#ifdef HAVE_OCULUS
        {
            QString arg = config().analogsParameters().trimmed();
            if (arg == "xbox") {
                _analogs.resize(8);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROculus);
                    _internals->oculusAnalogsEntity = 0;
                }
            } else if (arg == "controller-left") {
                _analogs.resize(4);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROculus);
                    _internals->oculusAnalogsEntity = 1;
                }
            } else if (arg == "controller-right") {
                _analogs.resize(4);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROculus);
                    _internals->oculusAnalogsEntity = 2;
                }
            } else {
                QVR_WARNING("device %s: invalid Oculus analogs parameter", qPrintable(id()));
            }
        }
#endif
        break;
    case QVR_Device_Analogs_OpenVR:
#ifdef HAVE_OPENVR
        {
            QString arg = config().analogsParameters().trimmed();
            if (arg == "controller-0") {
                _analogs.resize(3);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROpenVRSystem);
                    _internals->openVrAnalogsEntity = 0;
                }
            } else if (arg == "controller-1") {
                _analogs.resize(3);
                if (QVRManager::processIndex() == config().processIndex()) {
                    Q_ASSERT(QVROpenVRSystem);
                    _internals->openVrAnalogsEntity = 1;
                }
            } else {
                QVR_WARNING("device %s: invalid OpenVR analogs parameter", qPrintable(id()));
            }
        }
#endif
        break;
    case QVR_Device_Analogs_OSVR:
#ifdef HAVE_OSVR
        {
            QStringList args = config().analogsParameters().split(' ', QString::SkipEmptyParts);
            _analogs.resize(args.length());
            if (QVRManager::processIndex() == config().processIndex()) {
                Q_ASSERT(QVROsvrClientContext);
                for (int i = 0; i < _analogs.length(); i++) {
                    const QString& osvrPath = args[i];
                    OSVR_ClientInterface osvrInterface;
                    osvrClientGetInterface(QVROsvrClientContext, qPrintable(osvrPath), &osvrInterface);
                    if (!osvrInterface) {
                        QVR_WARNING("device %s: OSVR interface path %s does not exist",
                                qPrintable(id()), qPrintable(osvrPath));
                    }
                    _internals->osvrAnalogsInterfaces.append(osvrInterface);
                }
            }
        }
#endif
        break;
    }
}

QVRDevice::~QVRDevice()
{
    if (_internals) {
#ifdef HAVE_QGAMEPAD
        delete _internals->buttonsGamepad;
        if (_internals->analogsGamepad != _internals->buttonsGamepad)
            delete _internals->analogsGamepad;
#endif
#ifdef HAVE_VRPN
        delete _internals->vrpnTrackerRemote;
        delete _internals->vrpnAnalogRemote;
        delete _internals->vrpnButtonRemote;
#endif
#ifdef HAVE_OSVR
        // the OSVR interfaces do not need to be cleaned up,
        // this will happen when the OSVR context exits.
#endif
        delete _internals;
    }
}

const QVRDevice& QVRDevice::operator=(const QVRDevice& d)
{
    _index = d._index;
    _position = d._position;
    _orientation = d._orientation;
    _buttons = d._buttons;
    _analogs = d._analogs;
    return *this;
}

int QVRDevice::index() const
{
    return _index;
}

const QString& QVRDevice::id() const
{
    return config().id();
}

const QVRDeviceConfig& QVRDevice::config() const
{
    return QVRManager::config().deviceConfigs().at(index());
}

#ifdef HAVE_OCULUS
static QVector3D QVROculusConvert(const ovrVector3f& v)
{
    return QVector3D(v.x, v.y, v.z);
}
static QQuaternion QVROculusConvert(const ovrQuatf& q)
{
    return QQuaternion(q.w, q.x, q.y, q.z);
}
#endif

void QVRDevice::update()
{
    if (config().processIndex() == QVRManager::processIndex()) {
        bool wantVelocityCalculation = (config().trackingType() != QVR_Device_Tracking_None
                && config().trackingType() != QVR_Device_Tracking_Static);
        if (wantVelocityCalculation) {
            _internals->lastTimestamp = _internals->currentTimestamp;
            _internals->lastPosition = _position;
            _internals->lastOrientation = _orientation;
            _internals->currentTimestamp = QVRTimer.nsecsElapsed();
        }
#ifdef HAVE_QGAMEPAD
        if (_internals->buttonsGamepad) {
            _buttons[ 0] = _internals->buttonsGamepad->buttonUp();
            _buttons[ 1] = _internals->buttonsGamepad->buttonDown();
            _buttons[ 2] = _internals->buttonsGamepad->buttonLeft();
            _buttons[ 3] = _internals->buttonsGamepad->buttonRight();
            _buttons[ 4] = _internals->buttonsGamepad->buttonL1();
            _buttons[ 5] = _internals->buttonsGamepad->buttonR1();
            _buttons[ 6] = _internals->buttonsGamepad->buttonL2();
            _buttons[ 7] = _internals->buttonsGamepad->buttonR2();
            _buttons[ 8] = _internals->buttonsGamepad->buttonL3();
            _buttons[ 9] = _internals->buttonsGamepad->buttonR3();
            _buttons[10] = _internals->buttonsGamepad->buttonA();
            _buttons[11] = _internals->buttonsGamepad->buttonB();
            _buttons[12] = _internals->buttonsGamepad->buttonX();
            _buttons[13] = _internals->buttonsGamepad->buttonY();
            _buttons[14] = _internals->buttonsGamepad->buttonCenter();
            _buttons[15] = _internals->buttonsGamepad->buttonGuide();
            _buttons[16] = _internals->buttonsGamepad->buttonSelect();
            _buttons[17] = _internals->buttonsGamepad->buttonStart();
        }
        if (_internals->analogsGamepad) {
            _analogs[0] = _internals->analogsGamepad->axisRightY();
            _analogs[1] = _internals->analogsGamepad->axisRightX();
            _analogs[2] = _internals->analogsGamepad->axisLeftY();
            _analogs[3] = _internals->analogsGamepad->axisLeftX();
        }
#endif
#ifdef HAVE_VRPN
        if (_internals->vrpnTrackerRemote) {
            _internals->vrpnHaveVelocity = false;
            _internals->vrpnTrackerRemote->mainloop();
            if (_internals->vrpnHaveVelocity)
                wantVelocityCalculation = false;
        }
        if (_internals->vrpnButtonRemote)
            _internals->vrpnButtonRemote->mainloop();
        if (_internals->vrpnAnalogRemote)
            _internals->vrpnAnalogRemote->mainloop();
#endif
#ifdef HAVE_OCULUS
        if (_internals->oculusTrackedEntity >= 0) {
            if (_internals->oculusTrackedEntity == 0) {
                _position = QVROculusConvert(QVROculusTrackingState.HeadPose.ThePose.Position);
                _orientation = QVROculusConvert(QVROculusTrackingState.HeadPose.ThePose.Orientation);
                _velocity = QVROculusConvert(QVROculusTrackingState.HeadPose.LinearVelocity);
                _angularVelocity = QVROculusConvert(QVROculusTrackingState.HeadPose.AngularVelocity);
                wantVelocityCalculation = false;
            } else if (_internals->oculusTrackedEntity == 1) {
                _position = QVROculusConvert(QVROculusRenderPoses[0].Position);
                _orientation = QVROculusConvert(QVROculusRenderPoses[0].Orientation);
            } else if (_internals->oculusTrackedEntity == 2) {
                _position = QVROculusConvert(QVROculusRenderPoses[1].Position);
                _orientation = QVROculusConvert(QVROculusRenderPoses[1].Orientation);
# if (OVR_PRODUCT_VERSION >= 1)
            } else if (_internals->oculusTrackedEntity == 3) {
                _position = QVROculusConvert(QVROculusTrackingState.HandPoses[ovrHand_Left].ThePose.Position);
                _orientation = QVROculusConvert(QVROculusTrackingState.HandPoses[ovrHand_Left].ThePose.Orientation);
                _velocity = QVROculusConvert(QVROculusTrackingState.HandPoses[ovrHand_Left].LinearVelocity);
                _angularVelocity = QVROculusConvert(QVROculusTrackingState.HandPoses[ovrHand_Left].AngularVelocity);
                wantVelocityCalculation = false;
            } else if (_internals->oculusTrackedEntity == 4) {
                _position = QVROculusConvert(QVROculusTrackingState.HandPoses[ovrHand_Right].ThePose.Position);
                _orientation = QVROculusConvert(QVROculusTrackingState.HandPoses[ovrHand_Right].ThePose.Orientation);
                _velocity = QVROculusConvert(QVROculusTrackingState.HandPoses[ovrHand_Right].LinearVelocity);
                _angularVelocity = QVROculusConvert(QVROculusTrackingState.HandPoses[ovrHand_Right].AngularVelocity);
                wantVelocityCalculation = false;
# endif
            }
            // This Y offset moves the sitting user's eyes to a default standing height in the virtual world:
            _position.setY(_position.y() + QVRObserverConfig::defaultEyeHeight);
        }
# if (OVR_PRODUCT_VERSION >= 1)
        if (_internals->oculusButtonsEntity >= 0) {
            if (_internals->oculusButtonsEntity == 1) {
                _buttons[0] = QVROculusInputState.Thumbstick[ovrHand_Left].y > +0.5f;
                _buttons[1] = QVROculusInputState.Thumbstick[ovrHand_Left].y < -0.5f;
                _buttons[2] = QVROculusInputState.Thumbstick[ovrHand_Left].x > +0.5f;
                _buttons[3] = QVROculusInputState.Thumbstick[ovrHand_Left].x < -0.5f;
                _buttons[4] = QVROculusInputState.Buttons & ovrButton_X;
                _buttons[5] = QVROculusInputState.Buttons & ovrButton_Y;
                _buttons[6] = QVROculusInputState.Buttons & ovrButton_LShoulder;
                _buttons[7] = QVROculusInputState.Buttons & ovrButton_Enter;
            } else if (_internals->oculusButtonsEntity == 2) {
                _buttons[0] = QVROculusInputState.Thumbstick[ovrHand_Right].y > +0.5f;
                _buttons[1] = QVROculusInputState.Thumbstick[ovrHand_Right].y < -0.5f;
                _buttons[2] = QVROculusInputState.Thumbstick[ovrHand_Right].x > +0.5f;
                _buttons[3] = QVROculusInputState.Thumbstick[ovrHand_Right].x < -0.5f;
                _buttons[4] = QVROculusInputState.Buttons & ovrButton_A;
                _buttons[5] = QVROculusInputState.Buttons & ovrButton_B;
                _buttons[6] = QVROculusInputState.Buttons & ovrButton_RShoulder;
                _buttons[7] = QVROculusInputState.Buttons & ovrButton_Enter;
            } else {
                _buttons[0] = QVROculusInputState.Buttons & ovrButton_Up;
                _buttons[1] = QVROculusInputState.Buttons & ovrButton_Down;
                _buttons[2] = QVROculusInputState.Buttons & ovrButton_Left;
                _buttons[3] = QVROculusInputState.Buttons & ovrButton_Right;
                _buttons[4] = QVROculusInputState.Buttons & ovrButton_A;
                _buttons[5] = QVROculusInputState.Buttons & ovrButton_B;
                _buttons[6] = QVROculusInputState.Buttons & ovrButton_X;
                _buttons[7] = QVROculusInputState.Buttons & ovrButton_Y;
                _buttons[8] = QVROculusInputState.Buttons & ovrButton_LShoulder;
                _buttons[9] = QVROculusInputState.Buttons & ovrButton_RShoulder;
                _buttons[10] = QVROculusInputState.Buttons & ovrButton_Enter;
                _buttons[11] = QVROculusInputState.Buttons & ovrButton_Back;
            }
        }
        if (_internals->oculusAnalogsEntity >= 0) {
            if (_internals->oculusAnalogsEntity == 1) {
                _analogs[0] = QVROculusInputState.Thumbstick[ovrHand_Left].y;
                _analogs[1] = QVROculusInputState.Thumbstick[ovrHand_Left].x;
                _analogs[2] = QVROculusInputState.IndexTrigger[ovrHand_Left];
                _analogs[3] = QVROculusInputState.HandTrigger[ovrHand_Left];
            } else if (_internals->oculusAnalogsEntity == 2) {
                _analogs[0] = QVROculusInputState.Thumbstick[ovrHand_Right].y;
                _analogs[1] = QVROculusInputState.Thumbstick[ovrHand_Right].x;
                _analogs[2] = QVROculusInputState.IndexTrigger[ovrHand_Right];
                _analogs[3] = QVROculusInputState.HandTrigger[ovrHand_Right];
            } else {
                _analogs[0] = QVROculusInputState.Thumbstick[ovrHand_Left].y;
                _analogs[1] = QVROculusInputState.Thumbstick[ovrHand_Left].x;
                _analogs[2] = QVROculusInputState.Thumbstick[ovrHand_Right].y;
                _analogs[3] = QVROculusInputState.Thumbstick[ovrHand_Right].x;
                _analogs[4] = QVROculusInputState.IndexTrigger[ovrHand_Left];
                _analogs[5] = QVROculusInputState.IndexTrigger[ovrHand_Right];
                _analogs[6] = QVROculusInputState.HandTrigger[ovrHand_Right];
                _analogs[7] = QVROculusInputState.HandTrigger[ovrHand_Left];
            }
        }
# endif
#endif
#ifdef HAVE_OPENVR
        if (_internals->openVrTrackedEntity >= 0) {
            _orientation = QVROpenVRTrackedOrientations[_internals->openVrTrackedEntity];
            _position = QVROpenVRTrackedPositions[_internals->openVrTrackedEntity];
            if (QVROpenVRHaveTrackedVelocities[_internals->openVrTrackedEntity]) {
                _velocity = QVROpenVRTrackedVelocities[_internals->openVrTrackedEntity];
                _angularVelocity = QVROpenVRTrackedAngularVelocities[_internals->openVrTrackedEntity];
                wantVelocityCalculation = false;
            }
        }
        if (_internals->openVrButtonsEntity >= 0) {
            _buttons[0] = QVROpenVRControllerStates[_internals->openVrButtonsEntity].rAxis[0].y > +0.5f;
            _buttons[1] = QVROpenVRControllerStates[_internals->openVrButtonsEntity].rAxis[0].y < -0.5f;
            _buttons[2] = QVROpenVRControllerStates[_internals->openVrButtonsEntity].rAxis[0].x < -0.5f;
            _buttons[3] = QVROpenVRControllerStates[_internals->openVrButtonsEntity].rAxis[0].x > +0.5f;
            unsigned long buttonPressed = QVROpenVRControllerStates[_internals->openVrButtonsEntity].ulButtonPressed;
            _buttons[4] = buttonPressed & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu);
            _buttons[5] = buttonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip);
        }
        if (_internals->openVrAnalogsEntity >= 0) {
            _analogs[0] = QVROpenVRControllerStates[_internals->openVrAnalogsEntity].rAxis[0].y;
            _analogs[1] = QVROpenVRControllerStates[_internals->openVrAnalogsEntity].rAxis[0].x;
            _analogs[2] = QVROpenVRControllerStates[_internals->openVrAnalogsEntity].rAxis[1].x;
        }
#endif
#ifdef HAVE_OSVR
        if (_internals->osvrTrackedEye != -1 || _internals->osvrTrackingInterface) {
            OSVR_Pose3 pose;
            bool ok;
            if (_internals->osvrTrackedEye == 0) { // center eye
                ok = (osvrClientGetViewerPose(QVROsvrDisplayConfig, 0, &pose) == OSVR_RETURN_SUCCESS);
            } else if (_internals->osvrTrackedEye == 1) { // left eye
                ok = (osvrClientGetViewerEyePose(QVROsvrDisplayConfig, 0, 0, &pose) == OSVR_RETURN_SUCCESS);
            } else if (_internals->osvrTrackedEye == 2) { // right eye
                OSVR_EyeCount eyes;
                osvrClientGetNumEyesForViewer(QVROsvrDisplayConfig, 0, &eyes);
                int e = (eyes == 2 ? 1 : 0);
                ok = (osvrClientGetViewerEyePose(QVROsvrDisplayConfig, 0, e, &pose) == OSVR_RETURN_SUCCESS);
            } else { // _internals->osvrTrackingInterface
                struct OSVR_TimeValue timestamp;
                ok = (osvrGetPoseState(_internals->osvrTrackingInterface, &timestamp, &pose) == OSVR_RETURN_SUCCESS);
            }
            if (ok) {
                if (_internals->osvrTrackedEye >= 0 && pose.translation.data[1] < 1.1f) {
                    // Assume the user wears a HMD and sits (i.e. no room-scale VR).
                    // In this case, we apply an offset to a default standing observer,
                    // just as we do for Oculus Rift.
                    pose.translation.data[1] += QVRObserverConfig::defaultEyeHeight;
                }
                _position = QVector3D(pose.translation.data[0], pose.translation.data[1],
                        pose.translation.data[2]);
                _orientation = QQuaternion(pose.rotation.data[0], pose.rotation.data[1],
                        pose.rotation.data[2], pose.rotation.data[3]);
            }
        }
        if (_internals->osvrButtonsInterfaces.length() > 0) {
            OSVR_ButtonState state;
            struct OSVR_TimeValue timestamp;
            for (int i = 0; i < _buttons.length(); i++) {
                if (_internals->osvrButtonsInterfaces[i]
                        && osvrGetButtonState(_internals->osvrButtonsInterfaces[i],
                            &timestamp, &state) == OSVR_RETURN_SUCCESS) {
                    _buttons[i] = state;
                }
            }
        }
        if (_internals->osvrAnalogsInterfaces.length() > 0) {
            OSVR_AnalogState state;
            struct OSVR_TimeValue timestamp;
            for (int i = 0; i < _analogs.length(); i++) {
                if (_internals->osvrAnalogsInterfaces[i]
                        && osvrGetAnalogState(_internals->osvrAnalogsInterfaces[i],
                            &timestamp, &state) == OSVR_RETURN_SUCCESS) {
                    _analogs[i] = state;
                }
            }
        }
#endif
        if (wantVelocityCalculation && _internals->lastTimestamp >= 0) {
            qint64 usecs = (_internals->currentTimestamp - _internals->lastTimestamp) / 1000;
            double secs = usecs / 1e6;
            _velocity = (_position - _internals->lastPosition) / secs;
            _angularVelocity = QVRAngularVelocityFromDiffQuaternion(
                    _orientation * _internals->lastOrientation.conjugated(), secs);
        }
    }
}

QDataStream &operator<<(QDataStream& ds, const QVRDevice& d)
{
    ds << d._index << d._position << d._orientation << d._velocity << d._angularVelocity << d._buttons << d._analogs;
    return ds;
}

QDataStream &operator>>(QDataStream& ds, QVRDevice& d)
{
    ds >> d._index >> d._position >> d._orientation >> d._velocity >> d._angularVelocity >> d._buttons >> d._analogs;
    return ds;
}
