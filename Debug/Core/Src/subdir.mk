################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../Core/Src/app.cpp \
../Core/Src/config.cpp \
../Core/Src/config_sync.cpp \
../Core/Src/event_logger.cpp \
../Core/Src/power_control.cpp \
../Core/Src/warning.cpp 

C_SRCS += \
../Core/Src/adc.c \
../Core/Src/beeper.c \
../Core/Src/button.c \
../Core/Src/can_bus.c \
../Core/Src/fire.c \
../Core/Src/led.c \
../Core/Src/main.c \
../Core/Src/spif.c \
../Core/Src/stm32h5xx_hal_msp.c \
../Core/Src/stm32h5xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32h5xx.c 

C_DEPS += \
./Core/Src/adc.d \
./Core/Src/beeper.d \
./Core/Src/button.d \
./Core/Src/can_bus.d \
./Core/Src/fire.d \
./Core/Src/led.d \
./Core/Src/main.d \
./Core/Src/spif.d \
./Core/Src/stm32h5xx_hal_msp.d \
./Core/Src/stm32h5xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32h5xx.d 

OBJS += \
./Core/Src/adc.o \
./Core/Src/app.o \
./Core/Src/beeper.o \
./Core/Src/button.o \
./Core/Src/can_bus.o \
./Core/Src/config.o \
./Core/Src/config_sync.o \
./Core/Src/event_logger.o \
./Core/Src/fire.o \
./Core/Src/led.o \
./Core/Src/main.o \
./Core/Src/power_control.o \
./Core/Src/spif.o \
./Core/Src/stm32h5xx_hal_msp.o \
./Core/Src/stm32h5xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32h5xx.o \
./Core/Src/warning.o 

CPP_DEPS += \
./Core/Src/app.d \
./Core/Src/config.d \
./Core/Src/config_sync.d \
./Core/Src/event_logger.d \
./Core/Src/power_control.d \
./Core/Src/warning.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32H523xx -c -I../Core/Inc -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib/include" -I../Drivers/STM32H5xx_HAL_Driver/Inc -I../Drivers/STM32H5xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32H5xx/Include -I../Drivers/CMSIS/Include -I../TouchGFX/App -I../TouchGFX/target/generated -I../TouchGFX/target -I../Middlewares/ST/touchgfx/framework/include -I../TouchGFX/generated/fonts/include -I../TouchGFX/generated/gui_generated/include -I../TouchGFX/generated/images/include -I../TouchGFX/generated/texts/include -I../TouchGFX/generated/videos/include -I../TouchGFX/gui/include -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.cpp Core/Src/subdir.mk
	arm-none-eabi-g++ "$<" -mcpu=cortex-m33 -std=gnu++14 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32H523xx -c -I../Core/Inc -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib/include" -I../Drivers/STM32H5xx_HAL_Driver/Inc -I../Drivers/STM32H5xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32H5xx/Include -I../Drivers/CMSIS/Include -I../TouchGFX/App -I../TouchGFX/target/generated -I../TouchGFX/target -I../Middlewares/ST/touchgfx/framework/include -I../TouchGFX/generated/fonts/include -I../TouchGFX/generated/gui_generated/include -I../TouchGFX/generated/images/include -I../TouchGFX/generated/texts/include -I../TouchGFX/generated/videos/include -I../TouchGFX/gui/include -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib" -O0 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/adc.cyclo ./Core/Src/adc.d ./Core/Src/adc.o ./Core/Src/adc.su ./Core/Src/app.cyclo ./Core/Src/app.d ./Core/Src/app.o ./Core/Src/app.su ./Core/Src/beeper.cyclo ./Core/Src/beeper.d ./Core/Src/beeper.o ./Core/Src/beeper.su ./Core/Src/button.cyclo ./Core/Src/button.d ./Core/Src/button.o ./Core/Src/button.su ./Core/Src/can_bus.cyclo ./Core/Src/can_bus.d ./Core/Src/can_bus.o ./Core/Src/can_bus.su ./Core/Src/config.cyclo ./Core/Src/config.d ./Core/Src/config.o ./Core/Src/config.su ./Core/Src/config_sync.cyclo ./Core/Src/config_sync.d ./Core/Src/config_sync.o ./Core/Src/config_sync.su ./Core/Src/event_logger.cyclo ./Core/Src/event_logger.d ./Core/Src/event_logger.o ./Core/Src/event_logger.su ./Core/Src/fire.cyclo ./Core/Src/fire.d ./Core/Src/fire.o ./Core/Src/fire.su ./Core/Src/led.cyclo ./Core/Src/led.d ./Core/Src/led.o ./Core/Src/led.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/power_control.cyclo ./Core/Src/power_control.d ./Core/Src/power_control.o ./Core/Src/power_control.su ./Core/Src/spif.cyclo ./Core/Src/spif.d ./Core/Src/spif.o ./Core/Src/spif.su ./Core/Src/stm32h5xx_hal_msp.cyclo ./Core/Src/stm32h5xx_hal_msp.d ./Core/Src/stm32h5xx_hal_msp.o ./Core/Src/stm32h5xx_hal_msp.su ./Core/Src/stm32h5xx_it.cyclo ./Core/Src/stm32h5xx_it.d ./Core/Src/stm32h5xx_it.o ./Core/Src/stm32h5xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32h5xx.cyclo ./Core/Src/system_stm32h5xx.d ./Core/Src/system_stm32h5xx.o ./Core/Src/system_stm32h5xx.su ./Core/Src/warning.cyclo ./Core/Src/warning.d ./Core/Src/warning.o ./Core/Src/warning.su

.PHONY: clean-Core-2f-Src

