#include <cstdint>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <thread>
#include <atomic>
class CameraChannel
{
public:
    explicit CameraChannel(int cameraId);
    ~CameraChannel();

	typedef struct VideoMode {
		int width;      // 默认 public
        int height;     // 默认 public
        int fps;
		int stride;
		uint64_t sizeimg; //
	} VideoMode;

	typedef struct CamConfig {
		int width;
		int height;
		int fps;
		uint32_t fmt;
	} CamConfig;	

	typedef struct Frame {
		int width;
		int height;
		int stride;
		uint32_t fmt;
		uint64_t byteused;
	} Frame;

	
	//queryCameraCaps();
    bool openDevice(const std::string &devicePath);

    bool configureCam(int width, int height, uint32_t pixelFormat);
	bool getCurrentConfig();
    bool allocateDmaBuffers(int bufferCount);
    bool start();
    void stop();

private:
    void captureLoop();
    bool queueBuffer(int index);
    bool dequeueBuffer(int &index);

private:
    int m_cameraId = -1;
    int m_fd = -1;

    int m_width = 0;
    int m_height = 0;
    uint32_t m_pixelFormat = 0;

    std::atomic<bool> m_running{false};
    std::thread m_thread;
};