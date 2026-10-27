package android.telephony;

import android.telecom.PhoneAccountHandle;

import java.util.Collections;
import java.util.List;

/**
 * The container's android.telephony.TelephonyManager.
 *
 * The platform's class answers from the device, and on modern versions it
 * refuses to answer at all: since Android 10 the device identifiers are
 * inaccessible to ordinary applications, so getDeviceId() throws a
 * SecurityException. Whether it throws or returns null is decided by the
 * target SDK of the installed package -- and the installed package here is the
 * container, whose target is current, not the archive, whose target is
 * whatever it was built for in 2011. An application of that era calls
 * getDeviceId() on a thread with no handler for an exception that never used
 * to exist, and the process dies during its own startup.
 *
 * The guest class loader answers the platform name with this class, so an
 * archive sees a phone that is present, idle and offline, with stable synthetic
 * identifiers. The container is an offline runtime on a device whose telephony
 * data a guest has no business reading; what the guest actually wants is
 * "something stable that identifies this installation", which every one of
 * these getters returns.
 *
 * Only the surface an application of that era reaches for is here. Anything
 * else is missing exactly as it is missing from the platform's own class on a
 * current release, and the error is loud instead of hidden behind a
 * SecurityException.
 */
public class TelephonyManager {
    public static final int PHONE_TYPE_NONE = 0;
    public static final int PHONE_TYPE_GSM = 1;
    public static final int PHONE_TYPE_CDMA = 2;
    public static final int PHONE_TYPE_SIP = 3;

    public static final int NETWORK_TYPE_UNKNOWN = 0;
    public static final int NETWORK_TYPE_GPRS = 1;
    public static final int NETWORK_TYPE_EDGE = 2;
    public static final int NETWORK_TYPE_UMTS = 3;
    public static final int NETWORK_TYPE_CDMA = 4;
    public static final int NETWORK_TYPE_EVDO_0 = 5;
    public static final int NETWORK_TYPE_EVDO_A = 6;
    public static final int NETWORK_TYPE_1xRTT = 7;
    public static final int NETWORK_TYPE_HSDPA = 8;
    public static final int NETWORK_TYPE_HSUPA = 9;
    public static final int NETWORK_TYPE_HSPA = 10;
    public static final int NETWORK_TYPE_LTE = 13;

    public static final int SIM_STATE_UNKNOWN = 0;
    public static final int SIM_STATE_ABSENT = 1;
    public static final int SIM_STATE_PIN_REQUIRED = 2;
    public static final int SIM_STATE_PUK_REQUIRED = 3;
    public static final int SIM_STATE_NETWORK_LOCKED = 4;
    public static final int SIM_STATE_READY = 5;
    public static final int SIM_STATE_NOT_READY = 6;

    public static final int CALL_STATE_IDLE = 0;

    public static final int DATA_DISCONNECTED = 0;
    public static final int DATA_CONNECTING = 1;
    public static final int DATA_CONNECTED = 2;
    public static final int DATA_SUSPENDED = 3;

    public static final int DATA_ACTIVITY_NONE = 0;
    public static final int DATA_ACTIVITY_IN = 1;
    public static final int DATA_ACTIVITY_OUT = 2;
    public static final int DATA_ACTIVITY_INOUT = 3;

    // A 15-digit IMEI, as the platform reports for a GSM phone. All zeroes is
    // what the emulator has always answered with: it is clearly not a real
    // device, it is stable across launches, and nothing validates it.
    private static final String DEVICE_ID = "000000000000000";
    // Test MCC/MNC 001/01, the same network code a test SIM carries.
    private static final String OPERATOR = "00101";
    private static final String SUBSCRIBER_ID = "001010000000000";
    private static final String SIM_SERIAL = "89000000000000000000";

    public TelephonyManager() {}

    // --- device and subscriber identifiers ----------------------------------

    public String getDeviceId() {
        return DEVICE_ID;
    }

    public String getDeviceId(int slotIndex) {
        return DEVICE_ID;
    }

    public String getImei() {
        return DEVICE_ID;
    }

    public String getImei(int slotIndex) {
        return DEVICE_ID;
    }

    public String getMeid() {
        return DEVICE_ID;
    }

    public String getMeid(int slotIndex) {
        return DEVICE_ID;
    }

    public String getSubscriberId() {
        return SUBSCRIBER_ID;
    }

    public String getSimSerialNumber() {
        return SIM_SERIAL;
    }

    // The phone number is the one identifier the platform never invents: an
    // application that shows it has to cope with null, and a made-up number
    // would be worse than none.
    public String getLine1Number() {
        return null;
    }

    public String getVoiceMailNumber() {
        return null;
    }

    public String getVoiceMailAlphaTag() {
        return "";
    }

    public String getDeviceSoftwareVersion() {
        return null;
    }

    public String getGroupIdLevel1() {
        return null;
    }

    // --- network and SIM ----------------------------------------------------

    public String getNetworkOperator() {
        return OPERATOR;
    }

    public String getNetworkOperatorName() {
        return "";
    }

    public String getNetworkCountryIso() {
        return "";
    }

    public String getNetworkCountryIso(int slotIndex) {
        return "";
    }

    public String getSimOperator() {
        return OPERATOR;
    }

    public String getSimOperatorName() {
        return "";
    }

    public String getSimCountryIso() {
        return "";
    }

    public String getSimCarrierIdName() {
        return null;
    }

    public int getSimCarrierId() {
        return -1;
    }

    public String getNetworkSpecifier() {
        return null;
    }

    public int getNetworkType() {
        return NETWORK_TYPE_UMTS;
    }

    public int getDataNetworkType() {
        return NETWORK_TYPE_UMTS;
    }

    public int getVoiceNetworkType() {
        return NETWORK_TYPE_UMTS;
    }

    public int getNetworkSelectionMode() {
        return 0; // AUTOMATIC
    }

    public boolean isNetworkRoaming() {
        return false;
    }

    public boolean isDataEnabled() {
        return false;
    }

    public boolean isDataEnabledForReason(int reason) {
        return false;
    }

    // --- state --------------------------------------------------------------

    public int getPhoneType() {
        return PHONE_TYPE_GSM;
    }

    public int getCallState() {
        return CALL_STATE_IDLE;
    }

    public int getCallStateForSubscription() {
        return CALL_STATE_IDLE;
    }

    public int getDataState() {
        return DATA_DISCONNECTED;
    }

    public int getDataActivity() {
        return DATA_ACTIVITY_NONE;
    }

    public int getSimState() {
        return SIM_STATE_READY;
    }

    public int getSimState(int slotIndex) {
        return SIM_STATE_READY;
    }

    public boolean hasIccCard() {
        return true;
    }

    public boolean isSmsCapable() {
        return true;
    }

    public boolean isVoiceCapable() {
        return true;
    }

    public String getMmsUserAgent() {
        return "";
    }

    public String getMmsUAProfUrl() {
        return "";
    }

    public int getSubscriptionId() {
        return 1;
    }

    public int getSubscriptionId(PhoneAccountHandle accountHandle) {
        return 1;
    }

    // --- cell information ---------------------------------------------------

    public CellLocation getCellLocation() {
        return null;
    }

    public List<CellInfo> getAllCellInfo() {
        return Collections.emptyList();
    }

    public SignalStrength getSignalStrength() {
        return null;
    }

    /**
     * Registrations are accepted and forgotten: there is no call state to
     * report and no cell to report it from, and an application only needs the
     * call not to fail.
     */
    public void listen(PhoneStateListener listener, int events) {}

    public TelephonyManager createForSubscriptionId(int subId) {
        return this;
    }

    public TelephonyManager createForPhoneAccountHandle(PhoneAccountHandle accountHandle) {
        return this;
    }
}
