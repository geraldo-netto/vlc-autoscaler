// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_SCALER_STATUS_H
#define AUTOUPSCALE_SCALER_STATUS_H

typedef enum
{
    SCALER_PROCESS_OK = 0,
    SCALER_PROCESS_TRANSIENT = 1,
    SCALER_PROCESS_FATAL = 2,
} scaler_process_status_t;

static inline int scaler_process_needs_fallback(scaler_process_status_t status)
{
    return status == SCALER_PROCESS_FATAL;
}

static inline scaler_process_status_t scaler_process_lines_status(
    int written_lines, int expected_lines)
{
    return written_lines == expected_lines
        ? SCALER_PROCESS_OK : SCALER_PROCESS_TRANSIENT;
}

#endif /* AUTOUPSCALE_SCALER_STATUS_H */
