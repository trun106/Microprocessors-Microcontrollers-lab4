/*
 * sched.h
 *
 *  Created on: Nov 3, 2026
 *      Author: trungvu
 */

#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stdbool.h>

#define SCH_MAX_TASKS 50//maximum tasks

/* Returned when SCH_Add_Task() cannot add a task */
#define NO_TASK_ID SCH_MAX_TASKS

#define SCH_TICK_MS 10 //10ms hardwareTimer

#define SCH_INVALID_HEAP_POS 51//max is 50 so invalid

//error code
#define ERROR_SCH_TOO_MANY_TASKS       1
#define ERROR_SCH_CANNOT_DELETE_TASK   2
#define ERROR_SCH_INVALID_TASK         3

#define RETURN_NORMAL 0
#define RETURN_ERROR  1

typedef struct{
    void (*pTask)(void);//func pointer
    uint32_t Delay;//setTimer before while loop (store deadline, not remain tick)
    uint32_t Period;//setTimer inside while loop
    uint8_t RunMe;//flag
    uint32_t TaskID;//ID of task inside array of task
    uint32_t Sequence;//for heap to compare when there're 2 tasks with the same Delay
} sTask;

extern volatile uint8_t Error_code_G;

void SCH_Init(void);

void SCH_Update(void);//timer run

void SCH_Dispatch_Tasks(void);//check timer flag

uint32_t SCH_Add_Task(//set timer
    void (*pFunction)(void),
    uint32_t DELAY,
    uint32_t PERIOD
);

uint8_t SCH_Delete_Task(uint32_t taskID);

uint32_t get_time();

void SCH_Report_Status();

void SCH_Go_To_Sleep();

#endif /* INC_SCHED_H_ */
