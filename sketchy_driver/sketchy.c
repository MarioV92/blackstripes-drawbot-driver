//user: rpi - pw: linuxcnc ip: 192.168.0.102
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "sketchy.h"
#include "Model.h"
#include "bool.h"
#include "Config.h"
#include "sketchy-ipc.h"
#include "machine-settings.h"

#ifdef __PI__
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <bcm2835.h>
#include <native/task.h>
#include <native/timer.h>
#endif

#ifndef __PI__
#include "Preview.h"
#endif

#ifdef __PI__
RT_TASK draw_task;
RT_TASK watchdog_task;
#endif

#define RIGHT_CLOCK RPI_V2_GPIO_P1_11
#define RIGHT_DIR RPI_V2_GPIO_P1_12
#define LEFT_CLOCK RPI_V2_GPIO_P1_13
#define LEFT_DIR RPI_V2_GPIO_P1_15
#define SOLENOID RPI_V2_GPIO_P1_16

StepperMotorDir stepleft = stepperMotorDirNone;
StepperMotorDir stepright = stepperMotorDirNone;

StepperMotorDir leftdir = stepperMotorDirNone;
StepperMotorDir rightdir = stepperMotorDirNone;

SolenoidState solenoidstate = solenoidStateUp;
SolenoidState solenoid = solenoidStateUp;

#ifndef __PI__
Preview *PREVIEW;
Preview *PREVIEW_PEN_MOVE;
long stepCounter = 0;
#endif

const char *input_imagename;
int input_threshold;

static bool paused = false;

void sketchy_suspend(){

    if(!paused){
        updateDriverState(driverStatusCodePaused,"","");
        paused = true;
        printf("PAUSED\n");

#ifdef __PI__
        rt_task_suspend(&draw_task);
#else
        // on a non xenomai os this busy loop simulates rt_task_suspend
        while(1){
            DriverCommand *cmd = getCommand();
            if(cmd->commandCode != commandCodePause){
                sketchy_resume();
                return;
            }
        }

#endif
    }
}

void sketchy_resume(){

    if(paused){
        Model_resume();
#ifndef __PI__
        Preview_updateSpeed(PREVIEW, Config_maxDelay(), Config_minDelay());
#endif
        updateDriverState(driverSatusCodeBusy,"","");
        paused = false;
#ifdef __PI__
        printf("-rt_task_resume-\n");
        rt_task_resume(&draw_task);
#else
        printf("RESUME\n");
#endif

    }
}

#ifdef __PI__

// On the raspberry PI / xenomai the watchdog rt_task
// checks if the draw task needs to be resumed
void watch(){
    while(1){
        rt_task_wait_period(NULL);
        if(paused){
            DriverCommand *cmd = getCommand();
            if(cmd->commandCode != commandCodePause){
                sketchy_resume();
            }
        }
        rt_task_set_periodic(&watchdog_task, TM_NOW, 500000);
    }
}

#endif

void executeStep(Step *step){

    Point *p = Point_allocWithSteps(BOT->leftsteps,BOT->rightsteps);
    bool shouldDraw = (BOT->penMode == penModeManualDown);

#ifdef __PI__

    rt_task_wait_period(NULL);

    stepleft = step->leftengine;
    stepright = step->rightengine;

    if(shouldDraw){
        solenoid = solenoidStateDown;
    }else{
        solenoid = solenoidStateUp;
    }
        
    if(stepleft != leftdir){

#ifdef __VPLOTTER__

        if(stepleft == stepperMotorDirUp){
            bcm2835_gpio_write(LEFT_DIR, HIGH);
        }else if(stepleft == stepperMotorDirDown){
            bcm2835_gpio_write(LEFT_DIR, LOW);
        }

#else
	//FOR MINI blackstripes with no gearboxes HIGH and LOW should be inverted for left only, NOT RIGHT!!
        //the gearboxes are mirrored to make the machine look better
        //so the direction signals have to be inverted
        if(stepleft == stepperMotorDirUp){
            bcm2835_gpio_write(LEFT_DIR, LOW);
        }else if(stepleft == stepperMotorDirDown){
            bcm2835_gpio_write(LEFT_DIR, HIGH);
        }

#endif
        
        leftdir = stepleft;
        
    }
    
    if(stepright != rightdir){

        if(stepright == stepperMotorDirUp){
            bcm2835_gpio_write(RIGHT_DIR, LOW);
        }else if(stepright == stepperMotorDirDown){
            bcm2835_gpio_write(RIGHT_DIR, HIGH);
        }
        
        rightdir = stepright;
        
    }

    if(solenoidstate != solenoid){

        if(solenoid == solenoidStateUp){
            bcm2835_gpio_write(SOLENOID, HIGH);
        }else if(solenoid == solenoidStateDown){
            bcm2835_gpio_write(SOLENOID, LOW);
        }

        solenoidstate = solenoid;

    }

    // sync the stepper steps //
    if (stepleft != stepperMotorDirNone) {
        bcm2835_gpio_write(LEFT_CLOCK, HIGH);
    }
    if (stepright != stepperMotorDirNone) {
        bcm2835_gpio_write(RIGHT_CLOCK, HIGH);
    }
    
    rt_task_sleep(100);
    
    if (stepleft != stepperMotorDirNone) {
        bcm2835_gpio_write(LEFT_CLOCK, LOW);
    }
    if (stepright != stepperMotorDirNone) {
        bcm2835_gpio_write(RIGHT_CLOCK, LOW);
    }
    
    rt_task_set_periodic(&draw_task, TM_NOW, BOT->delay);

#else

    int x = floor(p->x);
    int y = floor(p->y);
    Preview_setPixel(PREVIEW,x,y,BOT->delay, shouldDraw);
    Preview_setPixel(PREVIEW_PEN_MOVE,x,y,BOT->delay, !shouldDraw);
    stepCounter ++;
    if(stepCounter%10000 == 0){
        Preview_save(PREVIEW);
    }

#endif

    Point_release(p);
    
}

void catch_signal(int sig)
{
}

int run(void (*executeMotion)()){

    Model_createInstance();
    Model_setExecuteStepCallback(executeStep);
    Model_logState();
    
#ifdef __PI__
    
    signal(SIGTERM, catch_signal);
    signal(SIGINT, catch_signal);
    signal(SIGALRM, catch_signal);
    
    /* Avoids memory swapping for this program */
    mlockall(MCL_CURRENT|MCL_FUTURE);
    
    if (!bcm2835_init()){
        printf("error\n");
        //return 1;
    }
    
    bcm2835_gpio_fsel(RIGHT_CLOCK, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(RIGHT_DIR, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(LEFT_CLOCK, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(LEFT_DIR, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(SOLENOID, BCM2835_GPIO_FSEL_OUTP);

    bcm2835_gpio_write(SOLENOID, HIGH);
    rt_task_set_periodic(&draw_task, TM_NOW, BOT->delay);
    rt_task_set_periodic(&watchdog_task, TM_NOW, 500000);
    
    /*
     * Arguments: &task,
     *            name,
     *            stack size (0=default),
     *            priority,
     *            mode (FPU, start suspended, ...)
     */
    rt_task_create(&draw_task, "printerbot", 0, 99, 0);
    rt_task_create(&watchdog_task, "watchdog", 0, 99, 0);
    /*
     * Arguments: &task,
     *            task function,
     *            function argument
     */
    rt_task_start(&draw_task, executeMotion, NULL);
    rt_task_start(&watchdog_task, &watch, NULL);
    
    pause();
    
    rt_task_delete(&draw_task);
    rt_task_delete(&watchdog_task);
    
    bcm2835_gpio_write(SOLENOID, HIGH);
    
    
#else

    PREVIEW = Preview_alloc((int)MAX_CANVAS_SIZE_X,(int)MAX_CANVAS_SIZE_Y,"preview_image.png",Config_maxDelay(),Config_minDelay());
    PREVIEW_PEN_MOVE = Preview_alloc((int)MAX_CANVAS_SIZE_X,(int)MAX_CANVAS_SIZE_Y,"pen_move_image.png",Config_maxDelay(),Config_minMoveDelay());

    report_memory(1);
    executeMotion();
    Preview_save(PREVIEW);
    Preview_release(PREVIEW);
    Preview_save(PREVIEW_PEN_MOVE);
    Preview_release(PREVIEW_PEN_MOVE);

#endif

    Model_release();

    return 0;

}



