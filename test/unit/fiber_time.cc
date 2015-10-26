#include "memory.h"
#include "fiber.h"
#include "unit.h"

static void
delay_f(va_list ap)
{
	int d = 70000;
	usleep(d);
	fiber_sleep(0);
	d = (fiber()->fid - 101) * 100500;
	usleep(d);
	return;
}

static void
fiber_join_test()
{
	header();

	cord_start_profiling(cord());
	struct fiber *fib1 = fiber_new("fib1", delay_f);
	struct fiber *fib2 = fiber_new("fib2", delay_f);
	fiber_set_joinable(fib1, true);
	fiber_set_joinable(fib2, true);
	fiber_wakeup(fib1);
	fiber_wakeup(fib2);
	fiber_sleep(0);

	fiber_join(fib1);
	fiber_join(fib2);
	printf("fiber0 spent %d * 10^-2 seconds\n",
	       (int)(fiber()->time_spent + 5000000) / 10000000);
	printf("fiber1 spent %d * 10^-2 seconds\n",
	       (int)(fib1->time_spent + 5000000) / 10000000);
	printf("fiber2 spent %d * 10^-2 seconds\n",
	       (int)(fib2->time_spent + 5000000) / 10000000);

	footer();
}

static void
main_f(va_list ap)
{
	fiber_join_test();
	ev_break(loop(), EVBREAK_ALL);
}

int main()
{
	memory_init();
	fiber_init();
	struct fiber *main = fiber_new("main", main_f);
	fiber_wakeup(main);
	ev_run(loop(), 0);
	fiber_free();
	memory_free();
	return 0;
}
