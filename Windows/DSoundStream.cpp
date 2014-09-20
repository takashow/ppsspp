#include "native/thread/threadutil.h"
#include "Common/CommonWindows.h"
#include <dsound.h>

#include "dsoundstream.h"	

namespace WinAudio
{
#define BUFSIZE 0x4000
#define MAXWAIT 20   //ms

	int currentPos;
	int lastPos;
	short realtimeBuffer[BUFSIZE * 2];

	CRITICAL_SECTION soundCriticalSection;
	HANDLE soundSyncEvent = NULL;
	HANDLE hThread = NULL;

	IDirectSound8 *ds = NULL;
	IDirectSoundBuffer *dsBuffer = NULL;

	int bufferSize; // bytes
	int totalRenderedBytes;
	int sampleRate;

	volatile int threadData;

	inline int RoundDown128(int x) {
		return x & (~127);
	}

	void DSound::GetAudioFormat(AudioFormat *fmt) {
		fmt->sampleRateHz = sampleRate;
		fmt->numChannels = 2;
		fmt->sampleFormat = FMT_S16;
	}

	bool createBuffer() {
		PCMWAVEFORMAT pcmwf; 
		DSBUFFERDESC dsbdesc; 

		memset(&pcmwf, 0, sizeof(PCMWAVEFORMAT)); 
		memset(&dsbdesc, 0, sizeof(DSBUFFERDESC)); 

		pcmwf.wf.wFormatTag = WAVE_FORMAT_PCM; 
		pcmwf.wf.nChannels = 2; 
		pcmwf.wf.nSamplesPerSec = sampleRate = 44100; 
		pcmwf.wf.nBlockAlign = 4; 
		pcmwf.wf.nAvgBytesPerSec = pcmwf.wf.nSamplesPerSec * pcmwf.wf.nBlockAlign; 
		pcmwf.wBitsPerSample = 16; 

		dsbdesc.dwSize = sizeof(DSBUFFERDESC); 
		dsbdesc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS; // //DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY; 
		dsbdesc.dwBufferBytes = bufferSize = BUFSIZE;  //FIX32(pcmwf.wf.nAvgBytesPerSec);   //change to set buffer size
		dsbdesc.lpwfxFormat = (WAVEFORMATEX *)&pcmwf; 

		if (SUCCEEDED(ds->CreateSoundBuffer(&dsbdesc, &dsBuffer, NULL))) { 
			dsBuffer->SetCurrentPosition(0);
			return true; 
		} else { 
			dsBuffer = NULL; 
			return false; 
		} 
	}

	bool writeDataToBuffer(DWORD dwOffset, // Our own write cursor.
												 char* soundData, // Start of our data.
												 DWORD dwSoundBytes) {// Size of block to copy.
		void *ptr1, *ptr2;
		DWORD numBytes1, numBytes2; 
		// Obtain memory address of write block. This will be in two parts if the block wraps around.
		HRESULT hr = dsBuffer->Lock(dwOffset, dwSoundBytes, &ptr1, &numBytes1, &ptr2, &numBytes2, 0);
		if (SUCCEEDED(hr)) { 
			memcpy(ptr1, soundData, numBytes1); 
			if (ptr2!=0) 
				memcpy(ptr2, soundData+numBytes1, numBytes2); 
			dsBuffer->Unlock(ptr1, numBytes1, ptr2, numBytes2); 
			return true; 
		}
		return false; 
	} 

	inline int ModBufferSize(int x) {
		return (x+bufferSize)%bufferSize;
	}

	void DSound::soundThread() {
		setCurrentThreadName("DSound");
		currentPos = 0;
		lastPos = 0;

		dsBuffer->Play(0, 0, DSBPLAY_LOOPING);

		while (!threadData) {
			EnterCriticalSection(&soundCriticalSection);

			dsBuffer->GetCurrentPosition((DWORD *)&currentPos, 0);
			int numBytesToRender = RoundDown128(ModBufferSize(currentPos - lastPos));
			if (numBytesToRender >= 256) {
				int numBytesRendered = 4 * (*callback)(realtimeBuffer, numBytesToRender >> 2, 16, 44100, 2);
				//We need to copy the full buffer, regardless of what the mixer claims to have filled
				//If we don't do this then the sound will loop if the sound stops and the mixer writes only zeroes
				numBytesRendered = numBytesToRender;
				writeDataToBuffer(lastPos, (char *)realtimeBuffer, numBytesRendered);

				currentPos = ModBufferSize(lastPos + numBytesRendered);
				totalRenderedBytes += numBytesRendered;

				lastPos = currentPos;
			}

			LeaveCriticalSection(&soundCriticalSection);
			WaitForSingleObject(soundSyncEvent, MAXWAIT);
		}
		dsBuffer->Stop();

		threadData = 2;
	}

	unsigned int WINAPI DSound::soundThreadTrampoline(void *userdata) {
		DSound *dsound = (DSound *)userdata;
		dsound->soundThread();
		return 0;
	}

	bool DSound::StartSound(HWND window, StreamCallback _callback) {
		callback = _callback;
		threadData=0;

		soundSyncEvent=CreateEvent(0,false,false,0);  
		InitializeCriticalSection(&soundCriticalSection);

		if (FAILED(DirectSoundCreate8(0,&ds,0)))
			return false;

		ds->SetCooperativeLevel(window,DSSCL_PRIORITY);
		if (!createBuffer())
			return false;

		DWORD num1;
		short *p1; 

		dsBuffer->Lock(0, bufferSize, (void **)&p1, &num1, 0, 0, 0); 

		memset(p1,0,num1);
		dsBuffer->Unlock(p1,num1,0,0);
		totalRenderedBytes = -bufferSize;
		hThread = (HANDLE)_beginthreadex(0, 0, soundThreadTrampoline, (void *)this, 0, 0);
		SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
		return true;
	}

	void DSound::UpdateSound() {
		if (soundSyncEvent != NULL)
			SetEvent(soundSyncEvent);
	}

	void DSound::StopSound() {
		if (!dsBuffer)
			return;

		EnterCriticalSection(&soundCriticalSection);

		if (threadData == 0)
			threadData = 1;

		if (hThread != NULL) {
			WaitForSingleObject(hThread, 1000);
			CloseHandle(hThread);
			hThread = NULL;
		}

		if (threadData == 2) {
			if (dsBuffer != NULL)
				dsBuffer->Release();
			dsBuffer = NULL;
			if (ds != NULL)
				ds->Release();
			ds = NULL;
		}

		if (soundSyncEvent != NULL)
			CloseHandle(soundSyncEvent);
		soundSyncEvent = NULL;
		LeaveCriticalSection(&soundCriticalSection);
	}
}
