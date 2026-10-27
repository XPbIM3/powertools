/*
 * authprobe.c - диагностика AuthorizationExecuteWithPrivileges (Tiger)
 *
 * Печатает точный OSStatus вызова и код завершения целочисленного
 * /bin/sh -c 'echo ok > /tmp/authprobe.out'.  Запускать ТЕРМИНАЛОМ
 * на живой машине: /tmp/authprobe - появится password-диалог.
 */
#include <stdio.h>
#include <Security/Authorization.h>

static void prcode(const char *tag, OSStatus rc)
{
    printf("%s OSStatus = %ld (hex 0x%lx)", tag, (long)rc, (long)rc);
    switch (rc) {
    case errAuthorizationSuccess:            printf(" OK\n"); break;
    case errAuthorizationDenied:             printf("  (-60005 denied)\n"); break;
    case errAuthorizationCanceled:
        printf("  (-60006 canceled by user)\n"); break;
    case errAuthorizationInteractionNotAllowed:
        printf("  (-60007 interaction not allowed)\n"); break;
    case errAuthorizationToolExecuteFailure:
        printf("  (-60008 tool execute failure)\n"); break;
    default: printf("\n"); break;
    }
}

int main(void)
{
    AuthorizationRef auth = NULL;
    OSStatus rc;
    char *args[] = {(char *)"-c",
                    (char *)"echo ok > /tmp/authprobe.out",
                    NULL};
    FILE *fp = NULL;

    rc = AuthorizationCreate(NULL, NULL, kAuthorizationFlagDefaults,
                             &auth);
    prcode("AuthorizationCreate:", rc);
    if (rc != errAuthorizationSuccess) return 1;

    rc = AuthorizationExecuteWithPrivileges(auth, "/bin/sh",
        kAuthorizationFlagDefaults,      /* Tiger: options MUST be 0 */
        args, &fp);
    prcode("ExecuteWithPrivileges:", rc);
    if (fp != NULL)
        fclose(fp);

    {
        FILE *o = fopen("/tmp/authprobe.out", "r");
        printf("%s\n", o && fgetc(o) == 'o'
                       ? "RESULT: root tool RAN (out file created)"
                       : "RESULT: root tool did NOT run");
        if (o) fclose(o);
    }
    AuthorizationFree(auth, kAuthorizationFlagDefaults);
    return 0;
}
