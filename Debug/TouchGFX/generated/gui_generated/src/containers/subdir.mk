################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../TouchGFX/generated/gui_generated/src/containers/CustomContainerScrollTimeBase.cpp \
../TouchGFX/generated/gui_generated/src/containers/CustomContainerSollTextBase.cpp \
../TouchGFX/generated/gui_generated/src/containers/CustomContainerTimeBase.cpp \
../TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBarBase.cpp \
../TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBar_1Base.cpp \
../TouchGFX/generated/gui_generated/src/containers/mainmenuBase.cpp 

OBJS += \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerScrollTimeBase.o \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerSollTextBase.o \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerTimeBase.o \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBarBase.o \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBar_1Base.o \
./TouchGFX/generated/gui_generated/src/containers/mainmenuBase.o 

CPP_DEPS += \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerScrollTimeBase.d \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerSollTextBase.d \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerTimeBase.d \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBarBase.d \
./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBar_1Base.d \
./TouchGFX/generated/gui_generated/src/containers/mainmenuBase.d 


# Each subdirectory must supply rules for building sources it contributes
TouchGFX/generated/gui_generated/src/containers/%.o TouchGFX/generated/gui_generated/src/containers/%.su TouchGFX/generated/gui_generated/src/containers/%.cyclo: ../TouchGFX/generated/gui_generated/src/containers/%.cpp TouchGFX/generated/gui_generated/src/containers/subdir.mk
	arm-none-eabi-g++ "$<" -mcpu=cortex-m33 -std=gnu++14 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32H523xx -c -I../Core/Inc -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib/include" -I../Drivers/STM32H5xx_HAL_Driver/Inc -I../Drivers/STM32H5xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32H5xx/Include -I../Drivers/CMSIS/Include -I../TouchGFX/App -I../TouchGFX/target/generated -I../TouchGFX/target -I../Middlewares/ST/touchgfx/framework/include -I../TouchGFX/generated/fonts/include -I../TouchGFX/generated/gui_generated/include -I../TouchGFX/generated/images/include -I../TouchGFX/generated/texts/include -I../TouchGFX/generated/videos/include -I../TouchGFX/gui/include -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib" -O0 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-TouchGFX-2f-generated-2f-gui_generated-2f-src-2f-containers

clean-TouchGFX-2f-generated-2f-gui_generated-2f-src-2f-containers:
	-$(RM) ./TouchGFX/generated/gui_generated/src/containers/CustomContainerScrollTimeBase.cyclo ./TouchGFX/generated/gui_generated/src/containers/CustomContainerScrollTimeBase.d ./TouchGFX/generated/gui_generated/src/containers/CustomContainerScrollTimeBase.o ./TouchGFX/generated/gui_generated/src/containers/CustomContainerScrollTimeBase.su ./TouchGFX/generated/gui_generated/src/containers/CustomContainerSollTextBase.cyclo ./TouchGFX/generated/gui_generated/src/containers/CustomContainerSollTextBase.d ./TouchGFX/generated/gui_generated/src/containers/CustomContainerSollTextBase.o ./TouchGFX/generated/gui_generated/src/containers/CustomContainerSollTextBase.su ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTimeBase.cyclo ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTimeBase.d ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTimeBase.o ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTimeBase.su ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBarBase.cyclo ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBarBase.d ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBarBase.o ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBarBase.su ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBar_1Base.cyclo ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBar_1Base.d ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBar_1Base.o ./TouchGFX/generated/gui_generated/src/containers/CustomContainerTopBar_1Base.su ./TouchGFX/generated/gui_generated/src/containers/mainmenuBase.cyclo ./TouchGFX/generated/gui_generated/src/containers/mainmenuBase.d ./TouchGFX/generated/gui_generated/src/containers/mainmenuBase.o ./TouchGFX/generated/gui_generated/src/containers/mainmenuBase.su

.PHONY: clean-TouchGFX-2f-generated-2f-gui_generated-2f-src-2f-containers

