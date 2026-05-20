################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../TouchGFX/generated/images/src/BitmapDatabase.cpp \
../TouchGFX/generated/images/src/SVGDatabase.cpp \
../TouchGFX/generated/images/src/image_fire.cpp \
../TouchGFX/generated/images/src/image_fire_process.cpp \
../TouchGFX/generated/images/src/image_images.cpp \
../TouchGFX/generated/images/src/image_logo_small.cpp \
../TouchGFX/generated/images/src/image_logo_small1.cpp \
../TouchGFX/generated/images/src/image_material-symbols_wifi.cpp \
../TouchGFX/generated/images/src/image_no_beep.cpp \
../TouchGFX/generated/images/src/image_wifi.cpp 

OBJS += \
./TouchGFX/generated/images/src/BitmapDatabase.o \
./TouchGFX/generated/images/src/SVGDatabase.o \
./TouchGFX/generated/images/src/image_fire.o \
./TouchGFX/generated/images/src/image_fire_process.o \
./TouchGFX/generated/images/src/image_images.o \
./TouchGFX/generated/images/src/image_logo_small.o \
./TouchGFX/generated/images/src/image_logo_small1.o \
./TouchGFX/generated/images/src/image_material-symbols_wifi.o \
./TouchGFX/generated/images/src/image_no_beep.o \
./TouchGFX/generated/images/src/image_wifi.o 

CPP_DEPS += \
./TouchGFX/generated/images/src/BitmapDatabase.d \
./TouchGFX/generated/images/src/SVGDatabase.d \
./TouchGFX/generated/images/src/image_fire.d \
./TouchGFX/generated/images/src/image_fire_process.d \
./TouchGFX/generated/images/src/image_images.d \
./TouchGFX/generated/images/src/image_logo_small.d \
./TouchGFX/generated/images/src/image_logo_small1.d \
./TouchGFX/generated/images/src/image_material-symbols_wifi.d \
./TouchGFX/generated/images/src/image_no_beep.d \
./TouchGFX/generated/images/src/image_wifi.d 


# Each subdirectory must supply rules for building sources it contributes
TouchGFX/generated/images/src/%.o TouchGFX/generated/images/src/%.su TouchGFX/generated/images/src/%.cyclo: ../TouchGFX/generated/images/src/%.cpp TouchGFX/generated/images/src/subdir.mk
	arm-none-eabi-g++ "$<" -mcpu=cortex-m33 -std=gnu++14 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32H523xx -c -I../Core/Inc -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib/include" -I../Drivers/STM32H5xx_HAL_Driver/Inc -I../Drivers/STM32H5xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32H5xx/Include -I../Drivers/CMSIS/Include -I../TouchGFX/App -I../TouchGFX/target/generated -I../TouchGFX/target -I../Middlewares/ST/touchgfx/framework/include -I../TouchGFX/generated/fonts/include -I../TouchGFX/generated/gui_generated/include -I../TouchGFX/generated/images/include -I../TouchGFX/generated/texts/include -I../TouchGFX/generated/videos/include -I../TouchGFX/gui/include -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib" -O0 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-TouchGFX-2f-generated-2f-images-2f-src

clean-TouchGFX-2f-generated-2f-images-2f-src:
	-$(RM) ./TouchGFX/generated/images/src/BitmapDatabase.cyclo ./TouchGFX/generated/images/src/BitmapDatabase.d ./TouchGFX/generated/images/src/BitmapDatabase.o ./TouchGFX/generated/images/src/BitmapDatabase.su ./TouchGFX/generated/images/src/SVGDatabase.cyclo ./TouchGFX/generated/images/src/SVGDatabase.d ./TouchGFX/generated/images/src/SVGDatabase.o ./TouchGFX/generated/images/src/SVGDatabase.su ./TouchGFX/generated/images/src/image_fire.cyclo ./TouchGFX/generated/images/src/image_fire.d ./TouchGFX/generated/images/src/image_fire.o ./TouchGFX/generated/images/src/image_fire.su ./TouchGFX/generated/images/src/image_fire_process.cyclo ./TouchGFX/generated/images/src/image_fire_process.d ./TouchGFX/generated/images/src/image_fire_process.o ./TouchGFX/generated/images/src/image_fire_process.su ./TouchGFX/generated/images/src/image_images.cyclo ./TouchGFX/generated/images/src/image_images.d ./TouchGFX/generated/images/src/image_images.o ./TouchGFX/generated/images/src/image_images.su ./TouchGFX/generated/images/src/image_logo_small.cyclo ./TouchGFX/generated/images/src/image_logo_small.d ./TouchGFX/generated/images/src/image_logo_small.o ./TouchGFX/generated/images/src/image_logo_small.su ./TouchGFX/generated/images/src/image_logo_small1.cyclo ./TouchGFX/generated/images/src/image_logo_small1.d ./TouchGFX/generated/images/src/image_logo_small1.o ./TouchGFX/generated/images/src/image_logo_small1.su ./TouchGFX/generated/images/src/image_material-symbols_wifi.cyclo ./TouchGFX/generated/images/src/image_material-symbols_wifi.d ./TouchGFX/generated/images/src/image_material-symbols_wifi.o ./TouchGFX/generated/images/src/image_material-symbols_wifi.su ./TouchGFX/generated/images/src/image_no_beep.cyclo ./TouchGFX/generated/images/src/image_no_beep.d ./TouchGFX/generated/images/src/image_no_beep.o ./TouchGFX/generated/images/src/image_no_beep.su ./TouchGFX/generated/images/src/image_wifi.cyclo ./TouchGFX/generated/images/src/image_wifi.d ./TouchGFX/generated/images/src/image_wifi.o ./TouchGFX/generated/images/src/image_wifi.su

.PHONY: clean-TouchGFX-2f-generated-2f-images-2f-src

