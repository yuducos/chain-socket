#include "Poller.h"
#include "SystemReader.h"
#include "EpollPoller.hpp"
#include "SystemInterface.hpp"
#include "Timer.h"
#include "PrivateHeaders.h"

Poller::Poller(int port, int threadsNum) {
    this->port = port;
    this->maxWorker = threadsNum;
    sessions.resize(CONN_MAXFD);
    for (int i = 0; i < CONN_MAXFD; i++) {
        sessions[i] = (Session *) xmalloc(sizeof(Session));
        sessions[i]->reset();
        sessions[i]->sessionId = (uint64_t) i;
    }
}

Poller::~Poller() {
    this->isRunning = false;

    for (auto &E:workThreads) {
        if (E.joinable())
            E.join();
    }
    if (this->listenThread.joinable()) {
        this->listenThread.join();
    }
    for (int i = 0; i < CONN_MAXFD; i++) {
        sessions[i]->readBuffer.destroy();
        sessions[i]->writeBuffer.destroy();
    }

    for (int i = 0; i < this->maxWorker; i++) {
        this->workerVec[i]->poll->~SystemPoller();
        xfree(this->workerVec[i]->poll);
    }
    closeSocket(listenSocket);
}

void Poller::closeSession(Session &conn, int type) {
    if (conn.heartBeats == 0)
        return;
    int index = getPollIndex(conn.sessionId, this->maxWorker);
    Worker &woker = *this->workerVec[index];

    linger lingerStruct;
    lingerStruct.l_onoff = 1;
    lingerStruct.l_linger = 0;
    setSockOpt(conn.sessionId, SOL_SOCKET, SO_LINGER,
               &lingerStruct, sizeof(lingerStruct));
    conn.readBuffer.size = 0;
    conn.writeBuffer.size = 0;
    conn.heartBeats = 0;
    closeSocket(conn.sessionId);

    woker.onlineSessionSet.erase(&conn);
    woker.poll->pollDel(conn.sessionId);
    // TODO log std::cout << "close" << icc << std::endl;
    this->onDisconnect(conn, type);
}

int Poller::sendMsg(Session &conn, const Msg &msg) {
    if (conn.heartBeats == 0)
        return -1;
    int fd = conn.sessionId;
    int len = msg.len;
    unsigned char *data = msg.buff;
    if (conn.writeBuffer.size > 0) {
        conn.writeBuffer.push_back(len, data);
        return 0;
    }
    conn.writeBuffer.push_back(len, data);
    return 0;
}

int Poller::handleReadEvent(Session &conn) {
    if (conn.heartBeats <= 0 || conn.readBuffer.size > 1024 * 1024 * 1024)
        return -1;

    unsigned char *buff = conn.readBuffer.buff + conn.readBuffer.size;

    int ret = recv(conn.sessionId, (char *) buff, conn.readBuffer.capacity - conn.readBuffer.size, 0);

    if (ret > 0) {
        conn.readBuffer.size += ret;
        conn.readBuffer.alloc();
        conn.canRead = true;
        if (conn.readBuffer.size > 1024 * 1024 * 1024) {
            return -2;
            //TODO close socket
        }
        conn.heartBeats = HEARTBEATS_COUNT;

    } else if (ret == 0) {
        return -3;
    } else {
        if (!IsEagain()) {
            return -4;
        }
    }

    return 0;
}

int Poller::handleWriteEvent(Session &conn) {
    if (conn.heartBeats <= 0)
        return -1;
    if (conn.writeBuffer.size == 0)
        return 0;


    int ret = send(conn.sessionId, (const char *) conn.writeBuffer.buff,
                   conn.writeBuffer.size, 0);

    if (ret == -2) {

        if (!IsEagain()) {
            printf("err: write%d\n", getSockError());
            return -3;
        }

    } else if (ret == 0) {
        //TODO
    } else {
        onWriteBytes(conn, ret);
        conn.writeBuffer.erase(ret);
    }


    return 0;
}

void Poller::workerThreadCB(int pollIndex) {

    sockInfo task = {0};
    std::set<Session *> disconnectSet;
    bool isIdle = false;
    moodycamel::ConcurrentQueue<sockInfo> &queue = taskQueue[pollIndex];
    auto &worker = *this->workerVec[pollIndex];
    auto &poll = *worker.poll;
    auto &lock = worker.lock;
    auto &onlineSessionSet = worker.onlineSessionSet;

    while (this->isRunning) {
        if (isIdle)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lock.lock();
        while (queue.try_dequeue(task)) {
            if (task.event == CHECK_HEARTBEATS) {
                for (auto &E : onlineSessionSet) {
                    if (E->heartBeats <= 0)
                        continue;

                    if (E->heartBeats == 1) {
                        auto &conn = *this->sessions[E->sessionId];

                        disconnectSet.insert(&conn);
                    } else {
                        E->heartBeats--;
                    }
                }
                if (disconnectSet.size() > 0) {
                    for (auto &E :disconnectSet) {
                        E->heartBeats = -1;
                        this->workerVec[pollIndex]->evVec.emplace_back(std::pair<Session *, int>(E, REQ_DISCONNECT1));

                    }
                    disconnectSet.clear();
                }
            }
        }
        for (auto &E : onlineSessionSet) {
            Session &conn = *E;
            if (/*conn.ioPending == true || */conn.writeBuffer.size == 0)
                continue;
            int ret = send(conn.sessionId, (char *) conn.writeBuffer.buff, conn.writeBuffer.size, 0);

            if (ret > 0) {

                conn.writeBuffer.erase(ret);
                //conn.ioPending = false;
                //std::cout << "pending off" << std::endl;

            } else {
                if (!IsEagain()) {
                    if (conn.heartBeats > 0) {
                        E->heartBeats = -1;
                        std::cout << getSockError() << std::endl;
                        worker.evVec.emplace_back(std::pair<Session *, int>(E, REQ_DISCONNECT2));
                    }
                    continue;
                } else {
                    if (conn.writeBuffer.size > 0) {
                        int ev = EVENT_READ | EVENT_ONCE | EVENT_WRITE;
                        if (!conn.ioPending)
                            poll.pollMod(conn.sessionId, ev);//FIXME
                        conn.ioPending = true;
                        std::cout << "EVENT_WRITE MOD" << std::endl;
                    }
                }
            }
        }
        this->onIdle(pollIndex);
        worker.tm->DetectTimers();

        int numEvents = poll.pollGetEvents(MAX_EVENT);

        if (numEvents == -1) {

        } else if (numEvents > 0) {
            isIdle = false;
            for (int i = 0; i < numEvents; i++) {
                int sock = poll.getEventSocket(i);
                int evFlag = poll.getEventTrigerFlag(i);

                Session &conn = *this->sessions[sock];
                int ev = EVENT_READ | EVENT_ONCE;
                conn.ioPending = false;
                if (evFlag & EVENT_READ) {
                    int ret = 0;
                    if ((ret = this->handleReadEvent(conn)) < 0) {
                        if (conn.heartBeats > 0) {
                            conn.heartBeats = -1;
                            worker.evVec.emplace_back(
                                    std::pair<Session *, int>(sessions[sock], REQ_DISCONNECT4));
                        }
                        std::cout << "read" << ret << std::endl;
                        continue;
                    }
                }
                if (evFlag & EVENT_WRITE) {
                    std::cout << "EVENT_WRITE TRIGGERING" << std::endl;
                    int ret = 0;
                    if ((ret = this->handleWriteEvent(conn)) < 0) {
                        if (conn.heartBeats > 0) {
                            conn.heartBeats = -1;
                            worker.evVec.emplace_back(
                                    std::pair<Session *, int>(sessions[sock], REQ_DISCONNECT3));
                        }
                        std::cout << "write" << ret << std::endl;
                        continue;
                    } else {
                        if (conn.writeBuffer.size > 0) {
                            ev |= EVENT_WRITE;
                            conn.ioPending = true;
                            std::cout << "EVENT_WRITE MOD" << std::endl;
                        }
                    }
                }
                poll.pollMod(conn.sessionId, ev);//FIXME

            }
        } else if (numEvents == 0) {
            isIdle = true;
        }
        lock.unlock();
    }
}

void Poller::listenThreadCB() {

    poller = new(xmalloc(sizeof(SystemPoller))) SystemPoller(1);
    SystemPoller &poller = *this->poller;
    int ret = poller.pollAdd(this->listenSocket, EVENT_READ);
    while (this->isRunning) {

        std::this_thread::sleep_for(std::chrono::seconds(1));
        int numEvent = poller.pollGetEvents(1);
        if (numEvent > 0) {
            while (true) {
                int sock = accept(this->listenSocket, NULL, NULL);
                if (sock > 0) {
                    sockInfo x;
                    x.fd = sock;
                    x.event = ACCEPT_EVENT;
                    this->logicTaskQueue.enqueue(x);
                } else {
                    break;
                }
            }

        }
    }

}

bool Poller::createListenSocket(int port) {
    listenSocket = socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    setSockOpt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int nRcvBufferLen = 32 * 1024 * 1024;
    int nSndBufferLen = 32 * 1024 * 1024;
    int nLen = sizeof(int);

    setSockOpt(listenSocket, SOL_SOCKET, SO_SNDBUF, &nSndBufferLen, nLen);
    setSockOpt(listenSocket, SOL_SOCKET, SO_RCVBUF, &nRcvBufferLen, nLen);

    setSockNonBlock(listenSocket);
    struct sockaddr_in lisAddr;
    lisAddr.sin_family = AF_INET;
    lisAddr.sin_port = htons(port);
    lisAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listenSocket, (struct sockaddr *) &lisAddr, sizeof(lisAddr)) == -1) {
        printf("bind");
        return false;
    }

    if (::listen(listenSocket, 4096) < 0) {
        printf("listen");
        return false;
    }

    return true;
}

int Poller::run() {

    {/* init */
        this->isRunning = true;
    }
    {/* init queue  */
        taskQueue.resize(this->maxWorker);
    }
    {/* create listen*/
        this->createListenSocket(port);
    }
    for (int i = 0; i < this->maxWorker; i++) {
        Worker *worker = new(xmalloc(sizeof(Worker))) Worker();
        worker->index = i;
        workerVec.push_back(worker);
        worker->poll = new(xmalloc(sizeof(SystemPoller))) SystemPoller(MAX_EVENT);
        auto *tm1 = new(xmalloc(sizeof(TimerManager))) TimerManager();
        this->tm = new(xmalloc(sizeof(TimerManager))) TimerManager();
        worker->tm = tm1;

        this->onInit(i);
    }
    this->createTimerEvent(1000);

    {/* start workers*/
        for (int i = 0; i < this->maxWorker; ++i) {
            workThreads.emplace_back(std::thread([=] { this->workerThreadCB(i); }));
        }
    }

    {
        this->logicWorker = std::thread([=] { this->logicWorkerThreadCB(); });
    }

    {/* start listen*/
        listenThread = std::thread([=] { this->listenThreadCB(); });
    }

    this->heartBeatsThread = std::thread([this] {
        while (this->isRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEATS_INTERVAL * 1000));
            for (auto &E : this->taskQueue) {
                static sockInfo s;
                s.event = CHECK_HEARTBEATS;
                E.enqueue(s);
            }
        }

    });


    {/* wait exit*/
        this->heartBeatsThread.join();
        this->listenThread.join();
        this->logicWorker.join();
        for (auto &E: this->workThreads) {
            E.join();
        }
    }
    return 0;
}

int Poller::stop() {
    this->isRunning = false;
    return 0;
}

void Poller::logicWorkerThreadCB() {

    while (this->isRunning) {
        bool isIdle = true;

        for (int i = 0; i < this->maxWorker; i++) {
            this->workerVec[i]->lock.lock();
        }
        this->tm->DetectTimers();


        for (int i = 0; i < this->maxWorker; i++) {
            auto x = this->workerVec[i]->onlineSessionSet; //TODO
            for (auto &E2:x) {
                if (!E2->canRead)
                    continue;
                E2->canRead = false;
                isIdle = false;
                int readBytes = onReadMsg(*E2, E2->readBuffer.size);
                E2->readBuffer.size -= readBytes;//TODO size < 0
            }
            for (auto &E:this->workerVec[i]->evVec) {
                isIdle = false;
                Session *session = E.first;
                if (E.second == REQ_DISCONNECT1
                    || E.second == REQ_DISCONNECT2
                    || E.second == REQ_DISCONNECT3
                    || E.second == REQ_DISCONNECT4
                    || E.second == REQ_DISCONNECT5) {

                    this->closeSession(*session, E.second);
                }

            }
            this->workerVec[i]->evVec.clear();
        }
        sockInfo event = {0};
        while (logicTaskQueue.try_dequeue(event)) {
            switch (event.event) {
                case ACCEPT_EVENT: {
                    isIdle = false;
                    int ret = 0;
                    int clientFd = event.fd;
                    int index = getPollIndex(clientFd, maxWorker);
                    auto &poller = *this->workerVec[index]->poll;

                    int nRcvBufferLen = 1 * 1024;
                    int nSndBufferLen = 1 * 1024;
                    int nLen = sizeof(int);
                    setSockOpt(clientFd, SOL_SOCKET, SO_SNDBUF, &nSndBufferLen, nLen);
                    setSockOpt(clientFd, SOL_SOCKET, SO_RCVBUF, &nRcvBufferLen, nLen);

                    int opValue = 1;
#ifdef OS_DARWIN
                    if(0 > setSockOpt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &opValue, sizeof(opValue)))
                    {
                        std::cout << "err: SO_NOSIGPIPE" << std::endl;
                    }
#endif
                    int nodelay = 1;
                    if (setSockOpt(clientFd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                                   sizeof(nodelay)) < 0)
                        perror("err: nodelay\n");
                    setSockNonBlock(clientFd);
                    auto &conn = *sessions[clientFd];
                    conn.reset();
                    conn.heartBeats = HEARTBEATS_COUNT;
                    conn.sessionId = clientFd;

                    int ret2 = poller.pollAdd(clientFd, EVENT_READ);

                    this->workerVec[index]->onlineSessionSet.insert(&conn);
                    this->onAccept(*sessions[clientFd], Addr());


                    break;
                }
                default: {
                    break;
                }
            }
        }


        for (int i = 0; i < this->maxWorker; i++) {
            for (auto &E:this->workerVec[i]->onlineSessionSet) //TODO
            {

            }
        }


        for (int i = 0; i < this->maxWorker; i++) {
            this->workerVec[i]->lock.unlock();
        }
        if (isIdle)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

    }

    for (int i = 0; i < this->maxWorker; i++) {
        std::set<Session *> backset = this->workerVec[i]->onlineSessionSet;
        for (auto &E : backset) {
            this->closeSession(*E, REQ_DISCONNECT5);
        }
    }

}

int Poller::createTimerEvent(int inv) {
    auto *t2 = new(xmalloc(sizeof(Timer))) Timer(*this->tm);
    t2->data = (char *) "bbb";
    t2->Start([this, inv](void *data) {
        this->onTimerEvent(inv);
    }, inv);

    return 0;
}