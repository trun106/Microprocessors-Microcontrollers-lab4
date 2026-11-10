/*
 * sched.c
 *
 *  Created on: Nov 3, 2026
 *      Author: trungvu
 */

#include "main.h"
#include "sched.h"


static volatile sTask SCH_tasks_G[SCH_MAX_TASKS];//task array
volatile uint8_t Error_code_G = 0;
static volatile uint32_t tick = 0;//1 tick = 10ms
volatile uint32_t system_time_ms = 0;//hardware counter

//stack (LIFO) to store the list of free slot ID
static uint8_t SCH_free_slots[SCH_MAX_TASKS];
static uint8_t SCH_free_count = 0;

/*
 Temporarily mask the interrupt here so the MCU is not interrupted by the timer
 while it is constructing/changing the heap for the scheduler.
 The old mask state is saved so it can be restored afterwards.
 */
static uint32_t SCH_Enter_Critical(void){
    uint32_t primask = __get_PRIMASK();//primask curr 0 (interrupt normally)
    __disable_irq();//temp disable normal maskable interrupt, the tim2 still ticking normally -> set PRIMASK() to 1
    return primask;//primask store the prev state of mask (store 0)
}
static void SCH_Exit_Critical(uint32_t primask){
    __set_PRIMASK(primask);//restore the prev interrupt state
}


static bool SCH_Deadline_Reached(uint32_t deadline, uint32_t current_time){
    return (current_time >= deadline);//tick up for
}

//use heap for o(1) in SCH_Update() -> lowest delay first, o(log n) in SCH_Dispatch_Tasks()
//heap: i: parent -> left child: 2i+1, 2i+2, backtracking: j: child -> parent: (j-1)/2
static volatile uint32_t SCH_heap[SCH_MAX_TASKS];
static volatile uint32_t SCH_heap_pos[SCH_MAX_TASKS];
static volatile uint32_t SCH_heap_count = 0;//num of task in the heap
static uint32_t SCH_sequence_counter = 0;//sequence for handle many tasks with same delay -> lower seq -> high prior

static bool SCH_Task_Before(uint32_t taskA, uint32_t taskB)//compare two task's delay, to insert into the heap
{
    uint32_t deadlineA = SCH_tasks_G[taskA].Delay;
    uint32_t deadlineB = SCH_tasks_G[taskB].Delay;

    if (deadlineA < deadlineB){
        return 1;
    }
    if (deadlineA > deadlineB){
        return 0;
    }

    return (SCH_tasks_G[taskA].Sequence < SCH_tasks_G[taskB].Sequence);//compare seq when both delay equal
}

//swap two heap pos (parent with it's child if needed)
static void SCH_Heap_Swap(uint32_t posA, uint32_t posB){
    uint32_t temp;
    temp = SCH_heap[posA];
    SCH_heap[posA] = SCH_heap[posB];
    SCH_heap[posB] = temp;

    SCH_heap_pos[SCH_heap[posA]] = posA;
    SCH_heap_pos[SCH_heap[posB]] = posB;
}

//when insert the new task
static void reHeapUp(uint32_t position){
    uint32_t parent;
    while (position > 0){
        parent = (position - 1) / 2;
        if (!SCH_Task_Before(SCH_heap[position], SCH_heap[parent])){//if pos.Delay > parent.Delay -> stop reHeap
            break;
        }

        SCH_Heap_Swap(position, parent);
        position = parent;//loop again from the parent pos
    }
}

//when rm task at root (rm when SCH_Deadline_Reached(root) = 1)
static void reHeapDown(uint32_t position){
    uint32_t left;
    uint32_t right;
    uint32_t smallest;
    while(1){
        left = (2 * position) + 1;
        right = (2 * position) + 2;
        smallest = position;

        if ((left < SCH_heap_count) && SCH_Task_Before(SCH_heap[left], SCH_heap[smallest])){
            smallest = left;
        }
        if ((right < SCH_heap_count) && SCH_Task_Before(SCH_heap[right], SCH_heap[smallest])){
            smallest = right;
        }
        if (smallest == position){
            break;
        }

        SCH_Heap_Swap(position, smallest);
        position = smallest;//loop again from the child pos
    }
}


static uint32_t SCH_Heap_Insert(uint32_t taskID){
    uint32_t position;

    if (SCH_heap_count >= SCH_MAX_TASKS){//= because count start from 0
        return RETURN_ERROR;
    }
    position = SCH_heap_count;//insert from left to right so insert at the end of heap arr

    SCH_heap[position] = taskID;
    SCH_heap_pos[taskID] = position;
    SCH_heap_count++;

    reHeapUp(position);
    return RETURN_NORMAL;
}

//remove task from heap
static void SCH_Heap_Remove(uint32_t taskID){
    uint32_t position;
    uint32_t last_position;
    uint32_t last_task;

    position = SCH_heap_pos[taskID];
    if (position == SCH_INVALID_HEAP_POS){//invalid pos
        return;
    }

    last_position = SCH_heap_count - 1;
    last_task = SCH_heap[last_position];
    SCH_heap_count--;

    if (position < SCH_heap_count){
        SCH_heap[position] = last_task;//move last element to the rmming task pos and override it too
        SCH_heap_pos[last_task] = position;

        if ((position > 0) && SCH_Task_Before(SCH_heap[position], SCH_heap[(position - 1) / 2])){
            reHeapUp(position);
        }else{
            reHeapDown(position);
        }
    }

    SCH_heap_pos[taskID] = SCH_INVALID_HEAP_POS;//assign it with out of bound slot index (51 > max is 50)
}


static void SCH_Release_Task(uint32_t taskID){//for one shot task and SCH_Delete_Task()'s helper
    SCH_tasks_G[taskID].pTask = 0;
    SCH_tasks_G[taskID].Delay = 0;
    SCH_tasks_G[taskID].Period = 0;
    SCH_tasks_G[taskID].RunMe = 0;
    SCH_tasks_G[taskID].Sequence = 0;
    SCH_tasks_G[taskID].TaskID = taskID;
    SCH_free_slots[SCH_free_count] = taskID;//free ID will added to the queue
    SCH_free_count++;
}


void SCH_Init(){
    uint32_t Index;
    SCH_heap_count = 0;
    SCH_free_count = 0;
    tick = 0;
    system_time_ms = 0;
    SCH_sequence_counter = 0;
    Error_code_G = 0;
    for (Index = 0; Index < SCH_MAX_TASKS; Index++){
        SCH_tasks_G[Index].pTask = 0;
        SCH_tasks_G[Index].Delay = 0;
        SCH_tasks_G[Index].Period = 0;
        SCH_tasks_G[Index].RunMe = 0;
        SCH_tasks_G[Index].TaskID = Index;
        SCH_tasks_G[Index].Sequence = 0;
        SCH_heap_pos[Index] = SCH_INVALID_HEAP_POS;

        SCH_free_slots[Index] = SCH_MAX_TASKS - 1 - Index;//first store the largest ID so that the last return 0 to ensure first task will has ID = 0
        												  //because SCH_free_slots is stack (LIFO) -> taskID = SCH_free_slots[--SCH_free_count];
    }

    SCH_free_count = SCH_MAX_TASKS;
}


void SCH_Update(){
    uint32_t first_task;
    tick++;
    system_time_ms += SCH_TICK_MS;
    if (SCH_heap_count > 0){
        first_task = SCH_heap[0];
        if (SCH_Deadline_Reached(SCH_tasks_G[first_task].Delay, tick)){
            SCH_tasks_G[first_task].RunMe = 1;//when tick reach dl
        }else{
            SCH_tasks_G[first_task].RunMe = 0;
        }
    }
}


uint32_t SCH_Add_Task(void (*pFunction)(void), uint32_t DELAY, uint32_t PERIOD){
    uint8_t taskID;

    if (pFunction == 0){
        Error_code_G = ERROR_SCH_INVALID_TASK;
        return NO_TASK_ID;
    }
    uint32_t primask;
    primask = SCH_Enter_Critical();

    if (SCH_free_count == 0){
        Error_code_G = ERROR_SCH_TOO_MANY_TASKS;
        SCH_Exit_Critical(primask);
        return NO_TASK_ID;
    }

    taskID = SCH_free_slots[--SCH_free_count];//stack -> first store the largest ID so that the last return 0 to ensure first task will has ID = 0

    SCH_tasks_G[taskID].pTask = pFunction;
    SCH_tasks_G[taskID].Delay = tick + DELAY;//add tick here because some event might cause the task start later (like pushing the button at tick 30 then the task will start at 30 + dl)
    SCH_tasks_G[taskID].Period = PERIOD;
    SCH_tasks_G[taskID].RunMe = 0;
    SCH_tasks_G[taskID].TaskID = taskID;
    SCH_tasks_G[taskID].Sequence = SCH_sequence_counter++;
    SCH_Heap_Insert(taskID);//insert to heap

    SCH_Exit_Critical(primask);

    return taskID;
}


uint8_t SCH_Delete_Task(uint32_t taskID){
    uint32_t primask;
    if (taskID >= SCH_MAX_TASKS){
        Error_code_G = ERROR_SCH_CANNOT_DELETE_TASK;
        return RETURN_ERROR;
    }
    primask = SCH_Enter_Critical();

    if (SCH_tasks_G[taskID].pTask == 0)
    {
        Error_code_G = ERROR_SCH_CANNOT_DELETE_TASK;

        SCH_Exit_Critical(primask);

        return RETURN_ERROR;
    }
    SCH_Heap_Remove((uint8_t)taskID);
    SCH_Release_Task((uint8_t)taskID);
    SCH_Exit_Critical(primask);

    return RETURN_NORMAL;
}


void SCH_Dispatch_Tasks(){
    uint8_t taskID;
    uint32_t primask;
    uint32_t oldDeadline;
    uint32_t period;
    void (*taskFunction)(void);

    while (1){
        primask = SCH_Enter_Critical();
        if (SCH_heap_count == 0){
            SCH_Exit_Critical(primask);
            break;
        }

        taskID = SCH_heap[0];

        if (!SCH_Deadline_Reached(SCH_tasks_G[taskID].Delay,tick)){//if not due yet
            SCH_Exit_Critical(primask);
            break;
        }
        SCH_tasks_G[taskID].RunMe = 1;//flag
        taskFunction = SCH_tasks_G[taskID].pTask;
        period = SCH_tasks_G[taskID].Period;
        oldDeadline = SCH_tasks_G[taskID].Delay;
        SCH_Heap_Remove(taskID);//start to exe -> remove from the heap

        if (period != 0){//periodic task
            SCH_tasks_G[taskID].Delay = oldDeadline + period;
            SCH_tasks_G[taskID].RunMe = 0;
            SCH_tasks_G[taskID].Sequence = SCH_sequence_counter++;

            SCH_Heap_Insert(taskID);//insert back to the heap to continue the job
        }else{//one shot
            SCH_Release_Task(taskID);
        }

        SCH_Exit_Critical(primask);

        if (taskFunction != 0){//exe that function outside the critical section and outside the timer interrupt.
            taskFunction();
        }
    }

}



uint32_t get_time(void){
    return system_time_ms;
}
