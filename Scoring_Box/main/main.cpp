#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
    void app_main(void) {
        printf("Hello from main.cpp!\n");
        // Start your application code here
    }
}
