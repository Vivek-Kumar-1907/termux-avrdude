#include <cstdio>
#include <cstring>
#include <chrono>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <iostream>
#include <pty.h>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include "usbuart.h"
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <termios.h> // Make sure this is at the very top of your file!
#include <libusb-1.0/libusb.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifndef USBDEVFS_DETACH_DRIVER
#define USBDEVFS_DETACH_DRIVER _IOW('U', 22, struct usbdevfs_ioctl)
#endif

#define CONTROL_SOCK "/data/data/com.termux/files/home/.ptyserial.sock"

int setup_control_socket() {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_SOCK, sizeof(addr.sun_path)-1);
    
    // Remove stale socket from previous run
    unlink(CONTROL_SOCK);
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    
    if (listen(sockfd, 5) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }
    
    fprintf(stderr, "Control socket ready at %s\n", CONTROL_SOCK);
    return sockfd;
}

// Detect chip type from VID:PID
typedef enum {
    CHIP_FTDI,    // 0403:6001
    CHIP_CH340,   // 1a86:7523
    CHIP_CP210X,  // 10c4:ea60
    CHIP_CDCACM,  //
    CHIP_UNKNOWN
} chip_type_t;
static chip_type_t g_chip = CHIP_UNKNOWN;

void set_dtr_rts_ch340(int usb_dev_fd, int dtr, int rts) {
    struct usbdevfs_ctrltransfer ctrl;
    ctrl.bRequestType = 0x40;
    ctrl.bRequest = 0xA4;
    ctrl.wIndex = 0;
    ctrl.wLength = 0;
    ctrl.timeout = 1000;
    ctrl.data = NULL;

    // CH340 wValue: bits are active LOW
    // bit 5 = RTS (0 = asserted)
    // bit 6 = DTR (0 = asserted)
    uint16_t val = 0x00FF;
    if (!dtr) val &= ~(1 << 6);  // assert DTR (active low)
    if (!rts) val &= ~(1 << 5);  // assert RTS (active low)
    ctrl.wValue = val;

    if (ioctl(usb_dev_fd, USBDEVFS_CONTROL, &ctrl) < 0) {
        fprintf(stderr, "CH340 modem ctrl failed: %s\n", strerror(errno));
    } else {
        fprintf(stderr, "CH340 DTR=%d RTS=%d (wValue=0x%04X)\n", dtr, rts, val);
    }
}

void set_dtr_rts_cp210x(int usb_dev_fd, int dtr, int rts) {
    // CP210x uses bRequest=0x07 for modem control
    struct usbdevfs_ctrltransfer ctrl;
    ctrl.bRequestType = 0x41;
    ctrl.bRequest = 0x07;  // SET_MHS
    ctrl.wIndex = 0;
    ctrl.wLength = 0;
    ctrl.timeout = 1000;
    ctrl.data = NULL;
    
    // bits 0-1 = DTR/RTS values, bits 8-9 = masks
    uint16_t val = 0;
    val |= (1 << 8);  // DTR mask
    val |= (1 << 9);  // RTS mask
    if (dtr) val |= (1 << 0);
    if (rts) val |= (1 << 1);
    ctrl.wValue = val;
    
    if (ioctl(usb_dev_fd, USBDEVFS_CONTROL, &ctrl) < 0) {
        fprintf(stderr, "CP210x modem ctrl failed: %s\n", strerror(errno));
    }
}

void set_dtr_rts_ftdi(int usb_dev_fd, int dtr, int rts) {
    struct usbdevfs_ctrltransfer ctrl;
    ctrl.bRequestType = 0x40;
    ctrl.bRequest = 0x01;
    ctrl.wIndex = 0;
    ctrl.wLength = 0;
    ctrl.timeout = 1000;
    ctrl.data = NULL;
    
    // DTR
    ctrl.wValue = dtr ? 0x0101 : 0x0100;
    ioctl(usb_dev_fd, USBDEVFS_CONTROL, &ctrl);
    
    // RTS
    ctrl.wValue = rts ? 0x0202 : 0x0200;
    ioctl(usb_dev_fd, USBDEVFS_CONTROL, &ctrl);
    
    fprintf(stderr, "DTR=%d RTS=%d\n", dtr, rts);
}

void set_dtr_rts_cdcacm(int usb_dev_fd, int dtr, int rts) {
    // CDC-ACM SET_CONTROL_LINE_STATE
    // bRequestType = 0x21 (class, interface, host to device)
    // bRequest = 0x22 (SET_CONTROL_LINE_STATE)
    // wValue bit 0 = DTR, bit 1 = RTS
    struct usbdevfs_ctrltransfer ctrl;
    ctrl.bRequestType = 0x21;
    ctrl.bRequest = 0x22;
    ctrl.wIndex = 0;
    ctrl.wLength = 0;
    ctrl.timeout = 1000;
    ctrl.data = NULL;

    uint16_t val = 0;
    if (dtr) val |= (1 << 0);
    if (rts) val |= (1 << 1);
    ctrl.wValue = val;

    if (ioctl(usb_dev_fd, USBDEVFS_CONTROL, &ctrl) < 0) {
        fprintf(stderr, "CDC-ACM modem ctrl failed: %s\n", strerror(errno));
    }
}

// Unified dispatcher
void set_dtr_rts(int usb_dev_fd, int dtr, int rts) {
    fprintf(stderr, "DTR=%d RTS=%d\n", dtr, rts);
    switch (g_chip) {
        case CHIP_FTDI:   set_dtr_rts_ftdi(usb_dev_fd, dtr, rts);   break;
        case CHIP_CH340:  set_dtr_rts_ch340(usb_dev_fd, dtr, rts);  break;
        case CHIP_CP210X: set_dtr_rts_cp210x(usb_dev_fd, dtr, rts); break;
        case CHIP_CDCACM: set_dtr_rts_cdcacm(usb_dev_fd, dtr, rts); break;
        default:          set_dtr_rts_ftdi(usb_dev_fd, dtr, rts);   break;
    }
}

void detect_chip_type(int usb_dev_fd) {
    struct usbdevfs_ioctl cmd;
    // Read device descriptor via control transfer
    uint8_t desc[18];
    struct usbdevfs_ctrltransfer ctrl;
    ctrl.bRequestType = 0x80;  // device to host, standard, device
    ctrl.bRequest = 0x06;      // GET_DESCRIPTOR
    ctrl.wValue = 0x0100;      // device descriptor
    ctrl.wIndex = 0;
    ctrl.wLength = 18;
    ctrl.timeout = 1000;
    ctrl.data = desc;
    
    if (ioctl(usb_dev_fd, USBDEVFS_CONTROL, &ctrl) < 0) {
        fprintf(stderr, "Could not read device descriptor\n");
        return;
    }
    
    // desc[8:9] = VID, desc[10:11] = PID (little endian)
    uint16_t vid = desc[8]  | (desc[9]  << 8);
    uint16_t pid = desc[10] | (desc[11] << 8);
    
    fprintf(stderr, "Detected VID:PID %04X:%04X\n", vid, pid);
    
    if      (vid == 0x0403)                    g_chip = CHIP_FTDI;
    // CH340 / CH341 / CH9102
    else if (vid == 0x1A86 && pid == 0x7523) { g_chip = CHIP_CH340;  // CH340G
    } else if (vid == 0x1A86 && pid == 0x5523) { g_chip = CHIP_CH340;  // CH341
    } else if (vid == 0x1A86 && pid == 0x55D4) { g_chip = CHIP_CH340;  // CH9102
    } else if (vid == 0x1A86 && pid == 0x7522) { g_chip = CHIP_CH340;  // CH340B

    // CP210x (Silicon Labs)
    } else if (vid == 0x10C4 && pid == 0xEA60) { g_chip = CHIP_CP210X; // CP2102
    } else if (vid == 0x10C4 && pid == 0xEA70) { g_chip = CHIP_CP210X; // CP2105
    } else if (vid == 0x10C4 && pid == 0xEA71) { g_chip = CHIP_CP210X; // CP2108
    } else if (vid == 0x10C4 && pid == 0xEA61) { g_chip = CHIP_CP210X; // CP2103
    } else if (vid == 0x10C4 && pid == 0xEA63) { g_chip = CHIP_CP210X; // CP2104

    // CDC-ACM (Official Arduino boards with native USB)
    } else if (vid == 0x2341 && pid == 0x0069) { g_chip = CHIP_CDCACM; // Uno R4 Minima
    } else if (vid == 0x2341 && pid == 0x0068) { g_chip = CHIP_CDCACM; // Uno R4 WiFi
    } else if (vid == 0x2341 && pid == 0x0036) { g_chip = CHIP_CDCACM; // Leonardo
    } else if (vid == 0x2341 && pid == 0x0037) { g_chip = CHIP_CDCACM; // Micro
    } else if (vid == 0x2341 && pid == 0x003B) { g_chip = CHIP_CDCACM; // Due (programming port)
    } else if (vid == 0x2341 && pid == 0x003D) { g_chip = CHIP_CDCACM; // Due (native port)
    } else if (vid == 0x2341 && pid == 0x0042) { g_chip = CHIP_CDCACM; // Mega 2560 R3 (CDC)
    } else if (vid == 0x2341 && pid == 0x0043) { g_chip = CHIP_CDCACM; // Uno R3 (CDC variant)

    // SparkFun Pro Micro / Qduino
    } else if (vid == 0x1B4F && pid == 0x9205) { g_chip = CHIP_CDCACM; // Pro Micro 3.3V
    } else if (vid == 0x1B4F && pid == 0x9206) { g_chip = CHIP_CDCACM; // Pro Micro 5V
    } else if (vid == 0x1B4F && pid == 0x0015) { g_chip = CHIP_CDCACM; // Qduino Mini

    // Adafruit (CDC-ACM)
    } else if (vid == 0x239A) {                  g_chip = CHIP_CDCACM; // all Adafruit boards
    } else if (vid == 0x16C0 && pid == 0x0483) { g_chip = CHIP_CDCACM; }

    else {

        fprintf(stderr, "Unknown chip, defaulting to CH340 protocol\n");
        g_chip = CHIP_CH340;
    }

    const char* names[] = {"FTDI", "CH340", "CP210X", "CDC-ACM", "UNKNOWN"};
    fprintf(stderr, "Chip: %s\n", names[g_chip]);
}


static bool terminated = false;

struct socket_args {
    int sockfd;
    int usb_dev_fd;
};

void *socket_listener(void* arg) {
    socket_args *a = (socket_args*)arg;
    int sockfd = a->sockfd;
    int usb_dev_fd = a->usb_dev_fd;
    
    while (!terminated) {
        // Accept with timeout so we can check terminated flag
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sockfd, &fds);
        struct timeval tv = {1, 0}; // 1 second timeout
        
        int ready = select(sockfd+1, &fds, NULL, NULL, &tv);
        if (ready <= 0) continue;
        
        int client = accept(sockfd, NULL, NULL);
        if (client < 0) continue;
        
        // Read command
        char buf[64];
        memset(buf, 0, sizeof(buf));
        int n = read(client, buf, sizeof(buf)-1);
        close(client);
        
        if (n <= 0) continue;
        
        // Parse command
        // Format: "DTR=0,RTS=1"
        int dtr = -1, rts = -1;
        char* dtr_pos = strstr(buf, "DTR=");
        char* rts_pos = strstr(buf, "RTS=");
        
        if (dtr_pos) dtr = atoi(dtr_pos + 4);
        if (rts_pos) rts = atoi(rts_pos + 4);
        
        if (dtr >= 0 && rts >= 0) {
            set_dtr_rts(usb_dev_fd, dtr, rts);
        }
        if (strstr(buf, "RESET_AVR")) {
fprintf(stderr, "Chip type: %d (0=FTDI 1=CH340 2=CP210X)\n", g_chip);
    if (g_chip == CHIP_CH340) {
        set_dtr_rts(usb_dev_fd, 0, 0);
        usleep(250000);
        set_dtr_rts(usb_dev_fd, 1, 1);
    }
    else if(g_chip == CHIP_CDCACM) {
        set_dtr_rts(usb_dev_fd, 0, 0);
        usleep(50000);
        set_dtr_rts(usb_dev_fd, 1, 1);

    }
    else {
        // FTDI and CP210x
        set_dtr_rts(usb_dev_fd, 0, 0);
        usleep(50000);
        set_dtr_rts(usb_dev_fd, 1, 1);
    }
    fprintf(stderr, "AVR reset done\n");
}

if (strstr(buf, "RESET_ESP32")) {
    set_dtr_rts(usb_dev_fd, 0, 1);
    usleep(100000);
    set_dtr_rts(usb_dev_fd, 1, 0);
    usleep(50000);
    set_dtr_rts(usb_dev_fd, 0, 0);
    usleep(50000);
    fprintf(stderr, "ESP32 bootloader reset done\n");
}

if (strstr(buf, "RESET_ESP8266")) {
    set_dtr_rts(usb_dev_fd, 0, 1);
    usleep(100000);
    set_dtr_rts(usb_dev_fd, 1, 0);
    usleep(100000);
    set_dtr_rts(usb_dev_fd, 0, 1);
    fprintf(stderr, "ESP8266 bootloader reset done\n");
}
    }
    
    return NULL;
}


static void doexit(int signal) {
    terminated = true;
}

void show_err(int err) {
    fprintf(stderr,"err(%d)==%s\n", err, strerror(err));
}
using namespace usbuart;
using namespace std::chrono;

static inline bool is_good(int status) noexcept {
    return status == status_t::alles_gute;
}

static inline bool is_usable(int status) noexcept {
    return  status == (status_t::usb_dev_ok | status_t::read_pipe_ok)  ||
            status == (status_t::usb_dev_ok | status_t::write_pipe_ok) ||
            status == status_t::alles_gute;
}

int main(int argc, char** argv) {
    channel chnl = bad_channel;
    int master = -1;

    if( argc < 2 ) {
        fprintf(stderr,"Usage: termux-usb -e '%s <command> [command arguments]' <usb device address>\n",argv[0]);
        return -1;
    }
    const char* usb_fd_arg_str = argv[argc-1];
    int usb_dev_fd = -1;
    try {
       usb_dev_fd = std::stoi(usb_fd_arg_str);
    } catch (const std::invalid_argument& ia) {
         fprintf(stderr,"Error: Invalid USB file descriptor argument %s. Not a number.\n",usb_fd_arg_str);
         return EXIT_FAILURE;
    } catch (const std::out_of_range& oor) {
         fprintf(stderr,"Error: USB file descriptor argument %s out of range.\n",usb_fd_arg_str);
         return EXIT_FAILURE;
    }

    if (usb_dev_fd < 0) {
        fprintf(stderr,"Error: Invalid USB file descriptor value parsed: %d (must be non-negative).\n",usb_dev_fd);
        return EXIT_FAILURE;
    }


     char ptyname[256];
     pid_t pid = forkpty(&master, ptyname, NULL, NULL);
     if (pid < 0) {
        perror("Could not create pty");
        return EXIT_FAILURE;
    }
     if (pid == 0) {//child process
        close(master);
    if (argc> 2) {
         argv[argc-1] = NULL;
         execvp(argv[1], const_cast<char *const* >(argv+1));
     }
    wait(NULL);
    exit(0);
 }
// Parent process
 fprintf(stderr, "PTY created: %s\n",ptyname);

 int slave_fd = open(ptyname, O_RDWR| O_NOCTTY);
 if(slave_fd < 0){
    perror("Couldnt open slave PTY");
    return EXIT_FAILURE;
 }

 chnl.fd_read = master;
 chnl.fd_write = master;

 FILE *port_file = fopen("/data/data/com.termux/files/home/pty_port.txt", "w");
 if(port_file){
        fprintf(port_file, "%s", ptyname);
        fclose(port_file);
 }

 context::setloglevel(loglevel_t::info);

 //libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
 context ctx;

// --- START CLOCAL SETTINGS ---
// Assuming 'master' is your PTY file descriptor
    struct termios tty;
    if (tcgetattr(master, &tty) == 0) {
        cfmakeraw(&tty);           // The magic bullet: forces pure binary bypass
        tty.c_cflag &= ~CSTOPB;    // 1 stop bit
        tty.c_cflag &= ~CRTSCTS;   // Disable hardware flow control
        tty.c_cflag |= (CLOCAL | CREAD); // Ignore modem controls, enable reading
        tty.c_cc[VMIN]  = 1;       // Block until at least 1 byte is read
        tty.c_cc[VTIME] = 0;       // No read timeout
        tcsetattr(master, TCSANOW, &tty);
        fprintf(stderr, "PTY configured with CLOCAL (Blind Mode) enabled.\n");
    } else {
         fprintf(stderr, "Warning: Failed to get PTY attributes.\n");
    }
    int res, status = 0;
      // ==========================================
    // 1. KICK ANDROID OFF THE FTDI CHIP
    // ==========================================
     struct usbdevfs_ioctl disconnect;
      disconnect.ifno = 0;
      disconnect.ioctl_code = USBDEVFS_DISCONNECT;
      disconnect.data = NULL;
      if(ioctl(usb_dev_fd, USBDEVFS_IOCTL, &disconnect) < 0){
         fprintf(stderr, "Detach failed : %s\n", strerror(errno)); 
      }
      else{
         fprintf(stderr, "Kernel driver detached successfully.\n");
      }

      int iface = 0;
      if(0){
      if(ioctl(usb_dev_fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0){
         fprintf(stderr, "Claim interface: %s\n", strerror(errno));
      }
      else{
         fprintf(stderr, "Interface claimed via ioctl. \n");
      }
      }
      // ==========================================
     // 2. SETUP SERIAL PARAMS & ATTACH
 // ==========================================
      usbuart::eia_tia_232_info pi;
      pi.baudrate = 115200;
      pi.databits = 8;
      pi.stopbits = static_cast<usbuart::stop_bits_t>(0);
      pi.parity  = static_cast<usbuart::parity_t>(0);
      detect_chip_type(usb_dev_fd); 
      // Let the library claim the interface naturally! No manual ioctl claims here.
      res = ctx.attach(usb_dev_fd, (uint8_t)iface, chnl, pi);
      if (res < 0) {
          fprintf(stderr, "FATAL: Attach failed with code: %d\n", res);
      } else {
           fprintf(stderr, "USB Attached successfully! Bridge is NOW OPEN.\n");
           FILE* rf = fopen("/data/data/com.termux/files/home/bridge_ready.txt", "w");
           if(rf){
               fprintf(rf, "ready\n");
               fclose(rf);
           }
      }
      // ==========================================
      // 3. FTDI DTR AUTO-RESET FOR ARDUINO
      // ==========================================
      fprintf(stderr, "Triggering FTDI DTR Reset...\n");
      set_dtr_rts(usb_dev_fd, 0, 0);
      usleep(100000);
      set_dtr_rts(usb_dev_fd, 1, 1);

      
      // Setup control socket
      int sockfd = setup_control_socket();

      // Launch socket listener thread
      socket_args sargs = { sockfd, usb_dev_fd };
      pthread_t sock_thread;
      pthread_create(&sock_thread, NULL, socket_listener, &sargs);
      
      fprintf(stderr, "Arduino Reset! Ready for avrdude.\n");
 // ==========================================
      signal(SIGINT, doexit);
      signal(SIGQUIT, doexit);
      signal(SIGCHLD,doexit);

      int count_down = 10;
      int timeout = 0;

      while(!terminated && (res=ctx.loop(timeout)) >= -error_t::no_channel) {
          status = ctx.status(chnl);
          if( ! is_usable(status = ctx.status(chnl)) ){ 
              fprintf(stderr, "Channel no longer usable, status=%d\n", status);
              break;
          };
          if( res == -error_t::no_channel || ! is_good(status) ) {
              timeout = 100;
              fprintf(stderr,"Channel error:%d. Status=%d.\n",res,status);
              if( --count_down <= 0 ){
                  fprintf(stderr, "Too many erros, giving up.\n");
                  break;
              }
          }
      }

      // Cleanup
      terminated = true;
      pthread_join(sock_thread, NULL);
      close(sockfd);
      unlink(CONTROL_SOCK);

      signal(SIGINT, SIG_DFL);
      signal(SIGQUIT, SIG_DFL);
      signal(SIGCHLD, SIG_DFL);
      kill(pid, SIGTERM);

      fprintf(stderr,"USB status %d res %d\n", status, res);
      ctx.close(chnl);
      res = ctx.loop(1000);
      if( res < -error_t::no_channel ) {
         fprintf(stderr,"Terminated with error %d\n",-res);
      } else
         res = 0;
      close(master);
      return res;
}
