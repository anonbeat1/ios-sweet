#pragma once
/* 0 ok; <0 refused or failed (nothing sent for -10..-12, -1..-4). */
int rpmh_vreg_enable(const char *res, unsigned mv);
