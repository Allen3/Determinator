#if LAB >= 6

#include <inc/lib.h>

char *msg = "Now is the time for all good men to come to the aid of their party.";

void
umain(void)
{
	char buf[100];
	int i, pid, p[2];

	argv0 = "pipereadeof";

	if ((i=pipe(p)) < 0)
		panic("pipe: %e", i);

	if ((pid=fork()) < 0)
		panic("fork: %e", i);

	if (pid == 0) {
		printf("[%08x] pipereadeof close %d\n", env->env_id, p[1]);
		close(p[1]);
		printf("[%08x] pipereadeof readn %d\n", env->env_id, p[0]);
		i = readn(p[0], buf, sizeof buf-1);
		if (i < 0)
			panic("read: %e", i);
		buf[i] = 0;
		if (strcmp(buf, msg) == 0)
			printf("\npipe read closed properly\n");
		else
			printf("\ngot %d bytes: %s\n", i, buf);
		exit();
	} else {
		printf("[%08x] pipereadeof close %d\n", env->env_id, p[0]);
		close(p[0]);
		printf("[%08x] pipereadeof write %d\n", env->env_id, p[1]);
		if ((i=write(p[1], msg, strlen(msg))) != strlen(msg))
			panic("write: %e", i);
		close(p[1]);
	}
	wait(pid);

	argv0 = "pipewriteeof";
	if ((i=pipe(p)) < 0)
		panic("pipe: %e", i);

	if ((pid=fork()) < 0)
		panic("fork: %e", i);

	if (pid == 0) {
		close(p[0]);
		for(;;){
			printf(".");
			if(write(p[1], "x", 1) != 1)
				break;
		}
		printf("\npipe write closed properly\n");
	}
	close(p[0]);
	close(p[1]);
	wait(pid);

	printf("pipe tests passed\n");
}
#endif