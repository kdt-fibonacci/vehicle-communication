CXX = g++
CC  = gcc

TARGET = ota_client

CXXFLAGS = -std=c++17 -Wall
CFLAGS   = -Wall

INCLUDES = -Ilcd/includes

LIBS = \
-lcurl \
-lpaho-mqttpp3 \
-lpaho-mqtt3as \
-lpthread \
-lwiringPi \
-lssl \
-lcrypto

CPP_SRCS = \
main.cpp \
ota_comm.cpp \
ota_security.cpp \
ota_uds_engine.cpp

C_SRCS = \
lcd/monitor.c \
lcd/lcd_i2c_driver.c \
lcd/lcd_ui.c

OBJS = $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)