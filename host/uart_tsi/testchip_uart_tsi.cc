// Vendored from chipyard/generators/testchipip (BSD-3, see NOTICE) with a macOS port:
// Apple's termios lacks the Bxxx constants above 230400, so the baud rate is applied
// with IOSSIOSPEED instead of the Linux switch table.
#include "testchip_uart_tsi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#ifdef __APPLE__
#include <sys/ioctl.h>
#include <IOKit/serial/ioss.h>
#endif

#define PRINTF(...) printf("UART-TSI: " __VA_ARGS__);

#ifndef __APPLE__
static speed_t select_baud(uint64_t baud_rate) {
  switch (baud_rate) {
  case 1200: return B1200;
  case 1800: return B1800;
  case 2400: return B2400;
  case 4800: return B4800;
  case 9600: return B9600;
  case 19200: return B19200;
  case 38400: return B38400;
  case 57600: return B57600;
  case 115200: return B115200;
  case 230400: return B230400;
  case 460800: return B460800;
  case 500000: return B500000;
  case 576000: return B576000;
  case 921600: return B921600;
  case 1000000: return B1000000;
  case 1152000: return B1152000;
  case 1500000: return B1500000;
  case 2000000: return B2000000;
  case 2500000: return B2500000;
  case 3000000: return B3000000;
  case 4000000: return B4000000;
  default:
    PRINTF("Unsupported baud rate %llu\n", (unsigned long long)baud_rate);
    exit(1);
  }
}
#endif

testchip_uart_tsi_t::testchip_uart_tsi_t(int argc, char** argv,
					 char* ttyfile, uint64_t baud_rate,
					 bool verbose, bool do_self_check)
  : testchip_tsi_t(argc, argv, false), verbose(verbose), in_load_program(false), do_self_check(do_self_check) {

  if (baud_rate != 115200) {
    PRINTF("Warning: You selected a non-standard baudrate. This will only work if the HW was configured with this baud-rate\n");
  }

  ttyfd = open(ttyfile, O_RDWR | O_NOCTTY);
  if (ttyfd < 0) {
    PRINTF("Error %i from open: %s\n", errno, strerror(errno));
    exit(1);
  }

  // https://blog.mbedded.ninja/programming/operating-systems/linux/linux-serial-ports-using-c-cpp/
  struct termios tty;

  if (tcgetattr(ttyfd, &tty) != 0) {
    PRINTF("Error %i from tcgetaddr: %s\n", errno, strerror(errno));
    exit(1);
  }

  tty.c_cflag &= ~PARENB; // Clear parity bit, disabling parity (most common)
  tty.c_cflag &= ~CSTOPB; // Clear stop field, only one stop bit used in communication (most common)
  tty.c_cflag &= ~CSIZE;  // Clear all the size bits
  tty.c_cflag |= CS8; // 8 bits per byte (most common)
  tty.c_cflag &= ~CRTSCTS; // Disable RTS/CTS hardware flow control (most common)
  tty.c_cflag |= CREAD | CLOCAL; // Turn on READ & ignore ctrl lines (CLOCAL = 1)

  tty.c_lflag &= ~ICANON;
  tty.c_lflag &= ~ECHO; // Disable echo
  tty.c_lflag &= ~ECHOE; // Disable erasure
  tty.c_lflag &= ~ECHONL; // Disable new-line echo
  tty.c_lflag &= ~ISIG; // Disable interpretation of INTR, QUIT and SUSP

  tty.c_iflag &= ~(IXON | IXOFF | IXANY); // Turn off s/w flow ctrl
  tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL); // Disable any special handling of received bytes

  tty.c_oflag &= ~OPOST; // Prevent special interpretation of output bytes (e.g. newline chars)
  tty.c_oflag &= ~ONLCR; // Prevent conversion of newline to carriage return/line feed

  tty.c_cc[VTIME] = 0;
  tty.c_cc[VMIN] = 0;

#ifdef __APPLE__
  // Any standard placeholder; the exact rate is requested with IOSSIOSPEED below.
  cfsetispeed(&tty, B9600);
  cfsetospeed(&tty, B9600);
#else
  speed_t baud_sel = select_baud(baud_rate);
  cfsetispeed(&tty, baud_sel);
  cfsetospeed(&tty, baud_sel);
#endif

  // Save tty settings, also checking for error
  if (tcsetattr(ttyfd, TCSANOW, &tty) != 0) {
    PRINTF("Error %i from tcsetattr: %s\n", errno, strerror(errno));
  }

#ifdef __APPLE__
  speed_t speed = (speed_t)baud_rate;
  if (ioctl(ttyfd, IOSSIOSPEED, &speed) < 0) {
    PRINTF("Error %i from IOSSIOSPEED(%llu): %s\n", errno, (unsigned long long)baud_rate, strerror(errno));
    exit(1);
  }
#endif
};

bool testchip_uart_tsi_t::handle_uart() {
  std::vector<uint16_t> to_write;
  while (data_available()) {
     uint32_t d = recv_word();
     to_write.push_back(d);
     to_write.push_back(d >> 16);
  }

  uint8_t* buf = (uint8_t*) to_write.data();
  size_t write_size = to_write.size() * 2;
  size_t remaining = write_size;

  while (remaining > 0) {
    ssize_t written = write(ttyfd, buf + write_size - remaining, remaining);
    if (written < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      PRINTF("Error %i from write: %s\n", errno, strerror(errno));
      exit(1);
    }
    remaining = remaining - (size_t)written;
  }
  if (verbose) {
    for (size_t i = 0; i < to_write.size() * 2; i++) {
      PRINTF("Wrote %x\n", buf[i]);
    }
  }

  uint8_t read_buf[256];
  int n = read(ttyfd, &read_buf, sizeof(read_buf));
  if (n < 0) {
    PRINTF("Error %i from read: %s\n", errno, strerror(errno));
    exit(1);
  }
  for (int i = 0; i < n; i++) {
    read_bytes.push_back(read_buf[i]);
  }

  if (read_bytes.size() >= 4) {
    uint32_t out_data = 0;
    uint8_t* b = ((uint8_t*)&out_data);
    for (size_t i = 0; i < (sizeof(uint32_t) / sizeof(uint8_t)); i++) {
      b[i] = read_bytes.front();
      read_bytes.pop_front();
    }
    if (verbose) PRINTF("Read %x\n", out_data);
    send_word(out_data);
  }
  return data_available() || n > 0;
}

bool testchip_uart_tsi_t::check_connection() {
  sleep(1); // sleep for 1 second
  uint8_t rdata = 0;
  int n = read(ttyfd, &rdata, 1);
  if (n > 0) {
    PRINTF("Error: Reading unexpected data %c from UART. Abort.\n", rdata);
    exit(1);
  }
  return true;
}

void testchip_uart_tsi_t::load_program() {
  in_load_program = true;
  PRINTF("Loading program\n");
  testchip_tsi_t::load_program();
  PRINTF("Done loading program\n");
  in_load_program = false;
}

void testchip_uart_tsi_t::write_chunk(addr_t taddr, size_t nbytes, const void* src) {
  if (this->in_load_program) { PRINTF("Loading ELF %llx-%llx ... ", (unsigned long long)taddr, (unsigned long long)(taddr + nbytes)); }
  testchip_tsi_t::write_chunk(taddr, nbytes, src);
  while (this->handle_uart()) { }
  if (this->in_load_program) { printf("Done\n"); }

  if (this->do_self_check && this->in_load_program) {
    uint8_t rbuf[chunk_max_size()];
    const uint8_t* csrc = (const uint8_t*)src;
    PRINTF("Performing self check of region %llx-%llx ... ", (unsigned long long)taddr, (unsigned long long)(taddr + nbytes));
    read_chunk(taddr, nbytes, rbuf);
    for (size_t i = 0; i < nbytes; i++) {
      if (rbuf[i] != csrc[i]) {
        PRINTF("\nSelf check failed at address %llx readback %x != source %x\n", (unsigned long long)(taddr + i), rbuf[i], csrc[i]);
        while (handle_uart()) { }
        exit(1);
      }
    }
    printf("Done\n");
  }
}

int main(int argc, char* argv[]) {
  PRINTF("Starting UART-based TSI\n");
  PRINTF("Usage: ./uart_tsi +tty=/dev/pts/xx <PLUSARGS> <bin>\n");
  PRINTF("       ./uart_tsi +tty=/dev/ttyxx  <PLUSARGS> <bin>\n");
  PRINTF("       ./uart_tsi +tty=/dev/ttyxx  +no_hart0_msip +init_write=0x80000000:0xdeadbeef none\n");
  PRINTF("       ./uart_tsi +tty=/dev/ttyxx  +no_hart0_msip +init_read=0x80000000 none\n");
  PRINTF("       ./uart_tsi +tty=/dev/ttyxx  +selfcheck <bin>\n");
  PRINTF("       ./uart_tsi +tty=/dev/ttyxx  +baudrate=921600 <bin>\n");

  // Add the permissive flags in manually here
  std::vector<std::string> args;
  for (int i = 0; i < argc; i++) {
    bool is_plusarg = argv[i][0] == '+';
    if (is_plusarg) {
      args.push_back("+permissive");
      args.push_back(std::string(argv[i]));
      args.push_back("+permissive-off");
    } else {
      args.push_back(std::string(argv[i]));
    }
  }

  std::string tty;
  bool verbose = false;
  bool self_check = false;
  uint64_t baud_rate = 115200;
  for (std::string& arg : args) {
    if (arg.find("+tty=") == 0) {
      tty = std::string(arg.c_str() + 5);
    }
    if (arg.find("+verbose") == 0) {
      verbose = true;
    }
    if (arg.find("+selfcheck") == 0) {
      self_check = true;
    }
    if (arg.find("+baudrate=") == 0) {
      baud_rate = strtoull(arg.substr(10).c_str(), 0, 10);
    }
  }

  if (tty.size() == 0) {
    PRINTF("ERROR: Must use +tty=/dev/ttyxx to specify a tty\n");
    exit(1);
  }

  PRINTF("Attempting to open TTY at %s\n", tty.c_str());
  std::vector<std::string> tsi_args(args);
  std::vector<char*> tsi_argv(args.size());
  for (size_t i = 0; i < args.size(); i++)
    tsi_argv[i] = tsi_args[i].data();

  testchip_uart_tsi_t tsi((int)args.size(), tsi_argv.data(),
			  tty.data(), baud_rate,
			  verbose, self_check);
  PRINTF("Checking connection status with %s\n", tty.c_str());
  if (!tsi.check_connection()) {
    PRINTF("Connection failed\n");
    exit(1);
  } else {
    PRINTF("Connection succeeded\n");
  }
  while (!tsi.done()) {
    tsi.switch_to_host();
    tsi.handle_uart();
  }
  PRINTF("Done, shutting down, flushing UART\n");
  while (tsi.handle_uart()) {
    tsi.switch_to_host();
  }; // flush any inflight reads or writes
  PRINTF("WARNING: You should probably reset the target before running this program again\n");
  return tsi.exit_code();
}
