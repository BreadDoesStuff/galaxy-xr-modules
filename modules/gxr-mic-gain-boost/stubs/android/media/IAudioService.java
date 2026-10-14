package android.media;

import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;

import java.util.List;

/** Compile-only stub (hidden framework AIDL). Real class comes from framework.jar at runtime. */
public interface IAudioService extends IInterface {
    void registerRecordingCallback(IRecordingConfigDispatcher rcdb) throws RemoteException;

    List<AudioRecordingConfiguration> getActiveRecordingConfigurations() throws RemoteException;

    abstract class Stub extends android.os.Binder implements IAudioService {
        public static IAudioService asInterface(IBinder obj) {
            throw new UnsupportedOperationException("stub");
        }
    }
}
