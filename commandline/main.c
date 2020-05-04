/*
 * This file is part of the usefull_macros project.
 * Copyright 2018 Edward V. Emelianoff <eddy@sao.ru>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <usefull_macros.h>

#include "canbus.h"
#include "canopen.h"
#include "cmdlnopts.h"
#include "pusirobot.h"

static glob_pars *GP = NULL;  // for GP->pidfile need in `signals`

void signals(int sig){
    putlog("Exit with status %d", sig);
    DBG("Exit with status %d", sig);
    restore_console();
    if(GP->pidfile) // remove unnesessary PID file
        unlink(GP->pidfile);
    canbus_close();
    exit(sig);
}

void iffound_default(pid_t pid){
    ERRX("Another copy of this process found, pid=%d. Exit.", pid);
}

int main(int argc, char *argv[]){
    initial_setup();
    char *self = strdup(argv[0]);
    GP = parse_args(argc, argv);
    check4running(self, GP->pidfile);
    free(self);
    signal(SIGTERM, signals); // kill (-15) - quit
    signal(SIGHUP, SIG_IGN);  // hup - ignore
    signal(SIGINT, signals);  // ctrl+C - quit
    signal(SIGQUIT, signals); // ctrl+\ - quit
    signal(SIGTSTP, SIG_IGN); // ignore ctrl+Z
    if(GP->NodeID != 1){
        if(GP->NodeID < 1 || GP->NodeID > 127) ERRX("Node ID should be a number from 1 to 127");
    }
    if(GP->microsteps > 0 && (1 != __builtin_popcount(GP->microsteps) || GP->microsteps == 1)) // __builtin_popcount - amount of non-zero bits in uint
        ERRX("Wrong microstepping settings, should be 0 or 2^(1..8)");
    if(GP->absmove != INT_MIN || GP->relmove != INT_MIN){ // wanna move
        if(GP->absmove != INT_MIN && GP->relmove != INT_MIN)
            ERRX("ABSMOVE and RELMOVE can't be used together");
        if(GP->maxspeed == 0)
            ERRX("Set non-zero MAXSPEED");
    }

    if(GP->logfile) openlogfile(GP->logfile);
    putlog(("Start application..."));
    putlog("Try to open CAN bus device %s", GP->device);
    setserialspeed(GP->serialspeed);
    if(canbus_open(GP->device)){
        putlog("Can't open %s @ speed %d. Exit.", GP->device, GP->serialspeed);
        signals(1);
    }
    if(canbus_setspeed(GP->canspeed)){
        putlog("Can't set CAN speed %d. Exit.", GP->canspeed);
        signals(2);
    }

    //setup_con();
    // print current position and state
    int64_t i64;
    uint8_t ID = GP->NodeID;
    if(INT64_MIN != (i64 = SDO_read(&ERRSTATE, ID))){
        if(i64){
            red("ERRSTATE=%d\n", i64);
            uint8_t s = (uint8_t)i64;
            for(uint8_t i = 0; i < 8; ++i){
                const char *msg = errname(s, i);
                if(msg) red("\t%s\n", msg);
            }
            if(!GP->clearerr) ERRX("Error status is not zero");
            if(SDO_write(&ERRSTATE, ID, s) || 0 != (i64 = SDO_read(&ERRSTATE, ID))){
                ERRX("Can't clean error status");
            }
        }
    }
    if(INT64_MIN != (i64 = SDO_read(&DEVSTATUS, ID))){
        green("DEVSTATUS=%d\n", (int)i64);
        uint8_t s = (uint8_t)i64;
        if(s){
            for(uint8_t i = 0; i < 8; ++i){
                const char *msg = devstatus(s, i);
                if(msg) red("\t%s\n", msg);
            }
            if(s != BUSY_STATE && GP->clearerr && (SDO_write(&DEVSTATUS, ID, s) || 0 != (i64 = SDO_read(&DEVSTATUS, ID)))){
                ERRX("Can't clean device status");
            }
            if(i64 && i64 != BUSY_STATE) ERRX("Can't work in this state"); // DIE if !busy
        }
    }else ERRX("Can't get device status");
    if(GP->zeropos){
        i64 = 0;
        if(SDO_write(&POSITION, ID, i64))
            ERRX("Can't clear position counter");
    }
    uint16_t microstepping = 0;
    if(INT64_MIN != (i64 = SDO_read(&MICROSTEPS, ID))){
        if(GP->microsteps > -1 && GP->microsteps != (int) i64){
            DBG("Try to change microsteps");
            if(SDO_write(&MICROSTEPS, ID, GP->microsteps) || INT64_MIN == (i64 = SDO_read(&MICROSTEPS, ID)))
                ERRX("Can't change microstepping");
        }
        microstepping = (uint16_t) i64;
        green("MICROSTEPPING=%u\n", microstepping);
    }else ERRX("Can't get microstepping");
    if(INT64_MIN != (i64 = SDO_read(&POSITION, ID)))
        green("CURPOS=%d\n", (int)i64/microstepping);
    else ERRX("Can't read current position");
    if(INT64_MIN != (i64 = SDO_read(&MAXSPEED, ID))){
        DBG("abs=%d, rel=%d", GP->absmove, GP->relmove);
        if(i64 == 0 && (GP->absmove != INT_MIN || GP->relmove != INT_MIN) && (GP->maxspeed == INT_MIN || GP->maxspeed == 0))
            ERRX("Can't move when MAXSPEED==0");
        if(GP->maxspeed != INT_MIN){
            GP->maxspeed *= microstepping;
            if(GP->maxspeed < MAX_SPEED_MIN || GP->maxspeed > MAX_SPEED_MAX)
                ERRX("MAXSPEED should be from %d to %d", MAX_SPEED_MIN/microstepping, MAX_SPEED_MAX/microstepping);
            DBG("Try to change max speed");
            if(SDO_write(&MAXSPEED, ID, GP->maxspeed) || INT64_MIN == (i64 = SDO_read(&MAXSPEED, ID)))
                ERRX("Can't change max speed");
        }
        green("MAXSPEED=%d\n", (int)i64/microstepping);
    }else ERRX("Can't read max speed");

    if(GP->stop){
        if(SDO_write(&STOP, ID, 1))
            ERRX("Can't stop motor");
    }

    if(GP->absmove != INT_MIN){
        if(SDO_write(&ABSSTEPS, ID, GP->absmove*microstepping))
            ERRX("Can't move to absolute position %d", GP->absmove);
    }
    if(GP->relmove != INT_MIN && GP->relmove){
        uint8_t dir = 1;
        if(GP->relmove < 0){ // negative direction
            dir = 0;
            GP->relmove = -GP->relmove;
        }
        if(SDO_write(&ROTDIR, ID, dir) || INT64_MIN == (i64 = SDO_read(&ROTDIR, ID)))
            ERRX("Can't change rotation direction");
        DBG("i64=%ld, dir=%d", i64, dir);
        if(SDO_write(&RELSTEPS, ID, GP->relmove*microstepping))
            ERRX("Can't move to relative position %d", GP->relmove);
    }
#if 0
    CANmesg m;
    double t = dtime() - 10.;
    while(1){
        m.ID = 0; // read all
        if(!canbus_read(&m)){
            showM(&m);
            SDO *x = parseSDO(&m);
            if(x){
                printf("Get SDO, NID=%d, CCS=%d, idx=%d, subidx=%d, datalen=%d\n", x->NID, x->ccs, x->index, x->subindex, x->datalen);
            }
        }
        if(dtime() - t > 5.){
            t = dtime();
            green("Try to get status.. ");
            uint8_t s;
            if(SDO_readByte(0x6001, 0, &s, 1)) red("Err\n");
            else printf("got: 0x%X\n", s);
        }
    }
#endif
    signals(0);
    return 0;
}
