#include "bank.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "command.h"
#include "errors.h"

// The account balances are represented by an array.
static int *accounts = NULL;

// The number of accounts.
static int account_count = 0;

// The number of ATMs.
static int atm_count = 0;

// This is used just for testing.
int *get_accounts() { return accounts; }

// Performs a `write` call, checking for errors and handlings
// partial writes. If there was an error it returns ERR_PIPE_WRITE_ERR.
// Note: data is void * since the actual type being written does not matter.

static int checked_write(int fd, void *data, int n) {
  char *d = (char *)data;
  while (n > 0) {
    int result = write(fd, data, n);
    if (result >= 0) {
      // this approach handles both complete and partial writes
      d += result;
      n -= result;
    } else {
      error_msg(ERR_PIPE_WRITE_ERR, "could not write message to atm");
      return ERR_PIPE_WRITE_ERR;
    }
  }
  return SUCCESS;
}

// Performs a `read` call, checking for errors and handlings
// partial read. If there was an error it returns ERR_PIPE_READ_ERR.
// Note: data is void * since the actual type being read does not matter.

static int checked_read(int fd, void *data, int n) {
  char *d = (char *)data;
  while (n > 0) {
    int result = read(fd, data, n);

    if (result > 0) {
      // this approach handles both complete and partial reads
      d += result;
      n -= result;
      continue;
    }

    if (result == 0) {
      // indicates EOF
      return ERR_ATM_CLOSED;
    }

    error_msg(ERR_PIPE_READ_ERR, "could not read message from atm");
    return ERR_PIPE_READ_ERR;
  }
  return SUCCESS;
}

// Checks to make sure that the ATM id is a valid ID.

static int check_valid_atm(int atmid) {
  if (0 <= atmid && atmid < atm_count) {
    return SUCCESS;
  } else {
    error_msg(ERR_UNKNOWN_ATM, "message received from unknown ATM");
    return ERR_UNKNOWN_ATM;
  }
}

// Checks to make sure the account ID is a valid account ID.

static int check_valid_account(int accountid) {
  if (0 <= accountid && accountid < account_count) {
    return SUCCESS;
  } else {
    error_msg(ERR_UNKNOWN_ACCOUNT, "message received for unknown account");
    return ERR_UNKNOWN_ACCOUNT;
  }
}

// Opens a bank for business. It is provided the number of ATMs and
// the number of accounts.

void bank_open(int atm_cnt, int account_cnt) {
  atm_count = atm_cnt;
  // Create the accounts:
  accounts = (int *)malloc(sizeof(int) * account_cnt);
  for (int i = 0; i < account_cnt; i++) {
    accounts[i] = 0;
  }
  account_count = account_cnt;
}

// Closes a bank.

void bank_close() { free(accounts); }

// Dumps out the accounts balances.

void bank_dump() {
  for (int i = 0; i < account_count; i++) {
    printf("Account %d: %d\n", i, accounts[i]);
  }
}

// The `bank` function processes commands received from an ATM.  It
// processes the commands and makes the appropriate changes to the
// designated accounts if necessary.  For example, if it receives a
// DEPOSIT message it will update the `to` account with the deposit
// amount.  It then communicates back to the ATM with success or
// failure.

int bank(int atm_out_fd[], Command *cmd, int *atms_remaining) {
  cmd_t c;
  int i, f, t, a;
  Command bankcmd;

  cmd_unpack(cmd, &c, &i, &f, &t, &a);
  int result = SUCCESS;

  // TODO 1
  if(check_valid_atm(i) != SUCCESS)
    return ERR_UNKNOWN_ATM;
  
  // TODO 2
  if(c == CONNECT){
    MSG_OK(&bankcmd, 0, f, t, a);
    result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
  }
  if(c == EXIT){
    *atms_remaining -= 1;
    MSG_OK(&bankcmd, 0, f, t, a);
    result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
  }
  if(c == DEPOSIT){
    if(check_valid_account(t) != SUCCESS){
      MSG_ACCUNKN(&bankcmd, 0, t);
      result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
    }
    else{
      accounts[t] += a;
      MSG_OK(&bankcmd, 0, f, t, a);
      result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
    }
  }
  if(c == WITHDRAW){
    if(check_valid_account(f) != SUCCESS && accounts[f] >= a){
      MSG_NOFUNDS(&bankcmd, 0, f, a);
      result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
    }
    else{
      MSG_OK(&bankcmd, 0, f, t, a);
      result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
    }
  }
  if(c == TRANSFER){
    if(check_valid_account(t) != SUCCESS){
      MSG_ACCUNKN(&bankcmd, 0, t);
      result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
    }
    else if(check_valid_account(f) != SUCCESS && accounts[f] >= a){
      MSG_NOFUNDS(&bankcmd, 0, f, a);
      result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
    }
    else{
      MSG_OK(&bankcmd, 0, f, t, a);
      result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
    }
  }
  if(c == BALANCE){
    if(check_valid_account(f) != SUCCESS){
      MSG_ACCUNKN(&bankcmd, 0, t);
      result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
    }
    else{
      MSG_OK(&bankcmd, 0, f, t, accounts[f]);
      result = checked_write(atm_out_fd[i], &bankcmd, MESSAGE_SIZE);
    }
  }
  else{
    error_msg(ERR_UNKNOWN_CMD, "error: unknown command to bank");
    result = ERR_UNKNOWN_CMD;
  }
  return result;
}

static int scanner = -1;  // this scans ATMs circularly, for fairness

static struct pollfd *pollfds;  // the fds and conditions to await

// sets up an fd_set holding the fds on which we may receive
// messages from ATMs
static void set_up_poll(int bank_in_fd[]) {
  pollfds = (struct pollfd *)(malloc(sizeof(struct pollfd) * atm_count));
  for (int i = 0; i < atm_count; ++i) {
    pollfds[i].fd = bank_in_fd[i];
    pollfds[i].events = POLLIN;  // Note: can also return POLLHUP
    // no need to set revents - it's an output of poll
  }
}

// when an atm closes, we don't want to look for more
// input from it
static void note_atm_closed(int atm, int bank_in_fd[]) {
  pollfds[atm].fd = -1;  // causes poll to ignore it
  close(bank_in_fd[atm]);
}

// Using scanner as a roving number of an ATM to check for input,
// tries to find a fd whose bit is set in in_fds and return the
// corresponding ATM number.
static int find_ready_atm() {
  while (1) {
    int result = poll(pollfds, atm_count, -1);

    if (result < 0) {
      // error value returned by select
      printf("poll had an error ... stopping\n");
      return result;
    }

    if (result == 0) {
      // returned "early" -- ignore
      continue;
    }

    // at least one fd ready; find next one circularly
    for (int j = atm_count; --j >= 0;) {
      ++scanner;
      scanner %= atm_count;
      if (pollfds[scanner].revents) {
        // some event happened on this fd
        return scanner;
      }
    }
    // if we get here, we checked atm_count fds, so there is a problem
    printf("find_ready_atm: no ready fd when there should be one\n");
  }
}

// This simply repeatedly tries to read another command from the
// bank input fd (coming from any of the atms) and calls the bank
// function to process the message and develop and send a reply.
// It stops when there are no active atms.

int run_bank(int bank_in_fd[], int atm_out_fd[]) {
  Command cmd;

  int result = 0;
  int atms_remaining = atm_count;

  set_up_poll(bank_in_fd);

  while (atms_remaining != 0) {
    int found = find_ready_atm();
    if (found < 0) return found;

    // read input from (apparently) ready atm
    result = checked_read(bank_in_fd[found], &cmd, MESSAGE_SIZE);
    if (result == ERR_ATM_CLOSED) {
      note_atm_closed(found, bank_in_fd);
      continue;
    }

    if (result != SUCCESS) return result;

    result = bank(atm_out_fd, &cmd, &atms_remaining);

    if (result == ERR_UNKNOWN_ATM) {
      printf("received message from unknown ATM. Ignoring...\n");
      continue;
    }

    if (result != SUCCESS) {
      return result;
    }
    continue;
  }

  return SUCCESS;
}
