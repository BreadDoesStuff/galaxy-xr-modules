package android.media;

import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;

import java.util.List;

/** Compile-only stub (hidden framework AIDL). Real class comes from framework.jar at runtime. */
public interface IRecordingConfigDispatcher extends IInterface {
    void dispatchRecordingConfigChange(List<AudioRecordingConfiguration> configs) throws RemoteException;

    abstract class Stub extends android.os.Binder implements IRecordingConfigDispatcher {
        public Stub() { }

        @Override
        public IBinder asBinder() { return this; }
    }
}
