#pragma once

#include "session.h"

#define PWDFILE "/etc/ausftp/ftpusers"

int check_credentials(char* user, char* pass);

int dtp_open_active(ftp_session_t* sess);

void dtp_close(ftp_session_t* sess);
