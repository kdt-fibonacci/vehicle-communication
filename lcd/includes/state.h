#ifndef STATE_H
#define STATE_H

typedef enum
{
    OFF,
    IDLE,
    WAIT,
    READY,
    PENDING,
    DOWNLOAD,
    VERIFICATION,
    INSTALL,
    WAIT_ACTIVATION,
    ACTIVATION,
    RECOVERY,
    REPORTING
} STATE;


#endif