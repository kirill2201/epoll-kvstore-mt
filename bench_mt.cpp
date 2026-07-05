#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>

const int CLIENTS = 8;
const int REQS_PER_CLIENT = 50000;

std::atomic<double> total_rps{0.0};
std::mutex cout_mutex;

void client_thread()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        return;
    }

    const char* req = "SET a 1\n";
    char buf[256];
    
    for(int i = 0; i < 1000; i++)
    {
        ssize_t w = write(fd, req, strlen(req));
        ssize_t r = read(fd, buf, sizeof(buf));
        if(w <= 0 || r <= 0) { perror("io"); break; }
    }

    auto t0 = std::chrono::steady_clock::now();
    int done = 0;
    for(int i = 0; i < REQS_PER_CLIENT; i++)
    {
        ssize_t w = write(fd, req, strlen(req));
        ssize_t r = read(fd, buf, sizeof(buf));
        if( w <= 0 || r <= 0) { perror("io"); break; }
        done++;
    }
    
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1e9;

    double rps = done / sec;
    total_rps += rps;
    
    std::lock_guard<std::mutex> g(cout_mutex);
    std::cout << "client rps: " << (long)rps << "\n";

    close(fd);
}

int main()
{
    std::vector<std::thread> clients;

    for(int i = 0; i < CLIENTS; i++)
    {
        clients.emplace_back(client_thread);
    }
    
    for(auto& t : clients)
    {
        t.join();
    }
    
    std::cout << "aggregate throughput: " << (long)total_rps.load() << " req/sec\n";

    return 0;
}
