// Compile with -std=c++17 -pthread -Inpu-open/server and run without arguments.
#include "../../npu-open/host/npu_daemon_client.h"
#include <cassert>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

static void check_timeout(const char *value, uint64_t expected)
{
	pid_t pid = fork();
	assert(pid >= 0);
	if (!pid) {
		setenv("NPU_DAEMON_DELIVER_TIMEOUT_US", value, 1);
		_exit(reply_deliver_timeout_us() == expected ? 0 : 1);
	}
	int status;
	assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main()
{
	check_timeout("2147483648000", 10000000);
	check_timeout("-1", 10000000);
	check_timeout("30us", 10000000);
	check_timeout("40000", 40000);
	setenv("NPU_DAEMON_DELIVER_TIMEOUT_US", "40000", 1);
	int blocked[2], reading[2], size = 4096;
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, blocked) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, reading) == 0);
	assert(setsockopt(blocked[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof size) == 0);
	Client slow(blocked[0]), live(reading[0]);
	std::vector<char> data(250000, 'x');
	npu_hdr h = {NPU_MAGIC, NPU_PING, 0, 42, 0, uint32_t(data.size())};
	std::thread reader([&] {
		npu_hdr reply{};
		assert(recv(reading[1], &reply, sizeof reply, MSG_WAITALL) == sizeof reply && reply.tag == 42);
		std::vector<char> got(reply.len);
		assert(recv(reading[1], got.data(), got.size(), MSG_WAITALL) == ssize_t(got.size()) && got == data);
	});
	slow.send(h, data.data(), data.size());
	assert(slow.gone);
	slow.send(h, data.data(), data.size());
	live.send(h, data.data(), data.size());
	reader.join();
	assert(!live.gone);
}
