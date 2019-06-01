#ifndef MAIN_POLLER_H
#define MAIN_POLLER_H

#include "SystemReader.h"
#include "Buffer.hpp"
#include "NetStruct.h"

#if defined(OS_LINUX)

#include "EpollPoller.hpp"
#endif

#if defined(OS_DARWIN)

#include "KqueuePoller.hpp"
#endif

#if defined(OS_WINDOWS)

#include "SelectPoller.hpp"

#endif


#include "NetStruct.h"
#include "Buffer.hpp"
#include <functional>
#include "SystemInterface.hpp"
#include <mutex>

class TimerManager;


#if defined(OS_LINUX)
typedef EpollPoller SystemPoller;
#elif defined (OS_DARWIN)
typedef KqueuePoller SystemPoller;
#else 
typedef SelectPoller SystemPoller;
#endif

class Worker {
public:
    std::set<Session *> onlineSessionSet;
    int index;

    TimerManager *tm;
    std::mutex lock;
    std::vector<std::pair<Session *, int>> evVec;
    SystemPoller *poll;
};

class Poller {
public:
    Poller(int port, int threadsNum);

    virtual ~Poller();

    virtual int onAccept(Session &conn, const Addr &addr) { return 0; }

    virtual int onReadMsg(Session &conn, int bytesNum) { return bytesNum; }

    virtual int onWriteBytes(Session &conn, int len) { return 0; }

    virtual int onDisconnect(Session &conn, int type) { return 0; }

    virtual int onIdle(int pollIndex) { return 0; }

    virtual int onInit(int pollIndex) { return 0; }

    virtual int onTimerEvent(int interval) { return 0; }


    int sendMsg(Session &conn, const Msg &msg);

    int run();

    int stop();


    void closeSession(Session &conn, int type);

protected:
    int createTimerEvent(int inv);

    int handleReadEvent(Session &conn);

    int handleWriteEvent(Session &conn);

    void workerThreadCB(int pollIndex);

    void listenThreadCB();

    bool createListenSocket(int port);

    void logicWorkerThreadCB();

    std::vector<Session *> sessions;
    int maxWorker = 0;
    int listenSocket = 0;
    int port = 0;
    std::vector<std::thread> workThreads;
    std::thread listenThread;
    std::vector<moodycamel::ConcurrentQueue<sockInfo> > taskQueue;
    volatile bool isRunning = false;
    std::vector<Worker *> workerVec;
    std::thread heartBeatsThread;
    TimerManager *tm;
    std::thread logicWorker;
    moodycamel::ConcurrentQueue<sockInfo> logicTaskQueue;

    SystemPoller *poller = nullptr;
};

#endif //MAIN_POLLER_H
