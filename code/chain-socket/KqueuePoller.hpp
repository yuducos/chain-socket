#include "SystemReader.h"

#if defined(OS_DARWIN) && !defined(SELECT_SERVER)

#ifndef KQUEUEPOLLER_H
#define KQUEUEPOLLER_H
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

inline int getPollIndex(int id, int maxNum) {
#ifdef OS_WINDOWS
    int index = id / 4 % maxNum;
#else
    int index = id % maxNum;
#endif
    return index;
}

inline void PollInit() {
#ifdef OS_WINDOWS
    WORD ver = MAKEWORD(2, 2);
    WSADATA dat;
    WSAStartup(ver, &dat);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
}

class KqueuePoller {
public:
    KqueuePoller(int maxEvent) : maxEvent(maxEvent) {
        pollId = kqueue();
        event_list = (struct kevent *) xmalloc(maxEvent * sizeof(struct kevent));
    }

    ~KqueuePoller() {
        close(pollId);
        xfree(event_list);
    }

    int disableWrite(int sockFd)
    {
        return 0;
    }
    int pollMod(int sockFd, int flag/*EVENT_READ | EVENT_WRITE | EVENT_ONCE*/) {
        int flag1 = EV_ADD;
        if(flag & EVENT_ONCE)
            flag1 |= EV_ONESHOT;

        if(flag & EVENT_WRITE)
        {
            EV_SET(&event_set, sockFd, EVFILT_WRITE, flag1, 0, 0, NULL);
            if (kevent(pollId, &event_set, 1, NULL, 0, NULL) == -1) {
                printf("error\n");
            }
        }

        if(flag & EVENT_READ)
        {
            EV_SET(&event_set, sockFd, EVFILT_READ, flag1, 0, 0, NULL);
            if (kevent(pollId, &event_set, 1, NULL, 0, NULL) == -1) {
                printf("error\n");
            }
        }

        return 0;
    }

    int pollAdd(int sockFd, int flag/*EVENT_READ | EVENT_WRITE | EVENT_ONCE*/) {
        int flag1 = EV_ADD;
        if(flag & EVENT_ONCE)
            flag1 |= EV_ONESHOT;
        if(flag & EVENT_WRITE)
        {
            EV_SET(&event_set, sockFd, EVFILT_WRITE, flag1, 0, 0, NULL);
            if (kevent(pollId, &event_set, 1, NULL, 0, NULL) == -1) {
                printf("error\n");
            }
        }

        if(flag & EVENT_READ)
        {
            EV_SET(&event_set, sockFd, EVFILT_READ, flag1, 0, 0, NULL);
            if (kevent(pollId, &event_set, 1, NULL, 0, NULL) == -1) {
                printf("error\n");
            }
        }

        return 0;
    }

    int pollDel(int sockFd) {
        return 0;
    }

    int pollGetEvents(int maxEvent) {
        struct timespec timeout;
        timeout.tv_sec = 0;
        timeout.tv_nsec = 0;
        int numEvents = kevent(pollId, nullptr, 0, event_list, MAX_EVENT, &timeout);
        return numEvents;
    }

    int getEventSocket(int index) {
        return event_list[index].ident;
    }

    int getEventTrigerFlag(int index) {
        int retFlag = 0;
        if(event_list[index].flags & EVFILT_WRITE)
            retFlag |= EVENT_WRITE;
        if(event_list[index].flags & EVFILT_READ)
            retFlag |= EVENT_READ;
        return retFlag;
    }

public:
    int pollId = 0;
    struct kevent event_set;
    struct kevent * event_list;
    int maxEvent;
};

#endif /* KQUEUEPOLLER_H */
#endif
