#include "server.h"

int main() {
	signal(SIGTERM, handler);
	signal(SIGINT, handler);
	signal(SIGQUIT, handler);
	init_shm();
	initialize_online();
	server();
	return EXIT_SUCCESS;
}

void init_shm() {
	shm_fd = shm_open(shm, O_CREAT | O_RDWR, 777);
	ftruncate(shm_fd, shm_size);
	shm_p = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
}

void write_to_shm(const char* mess) {
	int i;
	for (i = 0; i < max_connections; i++) {
		if (users[i].present == 1) {
			sem_post(users[i].sem);
			strcpy(shm_p, mess);
		}
	}
}

void initialize_online() {
	int i;
	users = malloc(max_connections * sizeof(struct online));
	for (i = 0; i < max_connections; i++)
		users[i].present = 0;
}

void resize() {
	puts("Resizing");
	max_connections *= 2;
	users = realloc(users, max_connections * sizeof(struct online));
}

void remove_from_online(int pid) {
	int i;
	for (i = 0; i < max_connections; i++) {
		if (users[i].pid == pid) {
			users[i].present = 0;
			sem_unlink(users[i].sem_name);
			return;
		}
	}

}

void notify(int mess, int skipid) {
	int i;
	for (i = 0; i < max_connections; i++) {
		if (users[i].present == 1) {
			if (mess == 1 && users[i].pid != skipid) // notify all except current process about connecting user
				kill(users[i].pid, 30);
			else if (mess == 0 && users[i].pid != skipid) // notify all except current process about disconnecting user
				kill(users[i].pid, 10);
			else
				kill(users[i].pid, 16); // notify all users about disconnected server
		}
	}
}

void add_to_online(int pid) {
	int i = 0;
	if (active_connections == max_connections) {
		resize();
	}

	// Add online user at the first available spot
	while (users[i].present == 1 && active_connections < max_connections)
		i++;
	users[i].pid = pid;
	users[i].present = 1;
	sprintf(users[i].sem_name, "%d", pid);
	printf("id: %d, sem: %s\n", pid, users[i].sem_name);
	users[i].sem = sem_open(users[i].sem_name, O_CREAT, 777, 0);
}

void handler(int sig) {
	printf("\nReceived signal to close... Closing socket descriptor...\n");
	printf("Killing clients' processes... Goodbye!\n");
	close(sock);
	notify(2, -1);
	free(users);
	shm_unlink(shm);
	sleep(3);
	exit(1);
}

// Establishes connection and starts the calculating threads
void server() {
	static int client_sock_desc, c;
	struct sockaddr_in server, client;

	// Struct setup
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(8888);
	sock = socket(AF_INET, SOCK_STREAM, 0);

	if (sock == -1) {
		printf("Could not create socket\n");
	}
	puts("Socket created");

	if ((bind(sock, (struct sockaddr*) &server, sizeof(server))) < 0) {
		perror("Bind failed. Error");
		return;
	}
	puts("Bind done");

	listen(sock, 0);
	puts("Waiting for incoming connections...");
	c = sizeof(struct sockaddr_in);

	while ((client_sock_desc = accept(sock, (struct sockaddr *) &client,
			(socklen_t*) &c))) {
		puts("Connection accepted");
		active_connections++;
		notify(1, -1);
		printf("Active connections are: %d\n", active_connections);

		pthread_t thr;
		void* new_socket = (void*) client_sock_desc;

		// Making new thread for each new client
		if (pthread_create(&thr, NULL, thread_func, new_socket)) {
			perror("Could not create thread");
			return;
		}
	}
}

void deserialize_input(Calculation* msg, ssize_t len, uint8_t* buf, float* l,
		float* r, char* op, int* pid) {
	msg = calculation__unpack(NULL, len, buf);
	if (msg == NULL) {
		fprintf(stderr, "error unpacking incoming message\n");
		exit(1);
	}
	*l = msg->left;
	*op = msg->operation[0];
	*r = msg->right;
	*pid = msg->pid;
}

float calculate(float l, float r, char op) {
	switch (op) {
	case '+':
		l += r;
		break;
	case '-':
		l -= r;
		break;
	case '*':
		l *= r;
		break;
	case '/':
		l /= r;
		break;
	}
	return l;
}

void serialize_result(uint8_t* res_buf, Result* res, float calculated_result,
		ssize_t* res_len) {
	res->result = calculated_result;
	*res_len = result__get_packed_size(res);
	result__pack(res, res_buf);
}

void* thread_func(void* arg) {
	int socket_desc = (int) arg, proc_id, flag = 0;
	float left, right, result;
	char operation, message[100];

	Calculation msg = CALCULATION__INIT;
	Result res = RESULT__INIT;
	uint8_t calc_buf[1024],
	result_buf[1024];
	ssize_t len, result_len;

	len = recv(socket_desc, calc_buf, 1024, 0);
	deserialize_input(&msg, len, calc_buf, &left, &right, &operation, &proc_id);
	add_to_online(proc_id);
	while ((len = recv(socket_desc, calc_buf, 1024, 0))) {
		deserialize_input(&msg, len, calc_buf, &left, &right, &operation,
				&proc_id);
		result = calculate(left, right, operation);
		serialize_result(result_buf, &res, result, &result_len);
		sprintf(message, "Client %d calculated %f %c %f = %f\n", proc_id, left,
				operation, right, result);
		write_to_shm(message);
		send(socket_desc, result_buf, result_len, 0);
	}

	sprintf(message, "Client %d has disconnected\n", proc_id);
	remove_from_online(proc_id);
	notify(0, proc_id);

	close(socket_desc);
	active_connections--;
	printf("Active connections are: %d\n", active_connections);
	return NULL;
}
