#ifndef __SOUNDSTREAM_H__
#define __SOUNDSTREAM_H__

#include "Common/CommonWindows.h"
#include <Windows.h>

struct IDirectSound8;
struct IDirectSoundBuffer;

namespace WinAudio {

	enum AudioSampleFormat {
		FMT_S16,
		FMT_FLOAT32,
	};

	struct AudioFormat {
		int sampleRateHz;
		int numChannels;
		AudioSampleFormat sampleFormat;
	};

	typedef int(*StreamCallback)(short *buffer, int numSamples, int bits, int rate, int channels);

	class AudioBackend {
	public:
		virtual ~AudioBackend() {}

		virtual bool StartSound(HWND window, const AudioFormat &format, StreamCallback _callback) = 0;
		virtual void UpdateSound() {}
		virtual void StopSound() = 0;
	};

	class DSound : public AudioBackend {
	public:
		bool StartSound(HWND window, const AudioFormat &format, StreamCallback _callback);
		void UpdateSound();
		void StopSound();

	private:
		void soundThread();
		bool createBuffer(const AudioFormat &format);
		bool writeDataToBuffer(DWORD dwOffset, char* soundData, DWORD dwSoundBytes);
		static unsigned int WINAPI soundThreadTrampoline(void *);

		inline int ModBufferSize(int x) {
			return (x + bufferSize) % bufferSize;
		}

		AudioFormat format_;
		StreamCallback callback;
		volatile int threadData;

		int bufferSize; // bytes
		int totalRenderedBytes;
		int sampleRate;

		IDirectSound8 *ds = NULL;
		IDirectSoundBuffer *dsBuffer = NULL;
		enum {
			BUFSIZE = 0x4000,
			MAXWAIT = 20   //ms
		};

		int currentPos;
		int lastPos;
		short realtimeBuffer[BUFSIZE * 2];

		CRITICAL_SECTION soundCriticalSection;
		HANDLE soundSyncEvent = NULL;
		HANDLE hThread = NULL;

		inline int RoundDown128(int x) {
			return x & (~127);
		}
	};

}  // namespace

 
#endif //__SOUNDSTREAM_H__