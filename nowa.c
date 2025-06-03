#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>

#define NUM_HOUSES_TOTAL 5
#define NUM_OPERATIONS 2

typedef enum {
    MSG_STEAL_REQ,
    MSG_STEAL_REL,
    MSG_TERMINATE
} MessageType;

typedef struct {
    MessageType type;
    int timestamp;
    int sender_rank;
    int house_id;  // Dodano: identyfikator domu
} Message;

typedef struct {
    int timestamp;
    int rank;
} Request;

int compare_requests(const void* a, const void* b) {
    Request* r_a = (Request*)a;
    Request* r_b = (Request*)b;
    if (r_a->timestamp != r_b->timestamp) {
        return r_a->timestamp - r_b->timestamp;
    }
    return r_a->rank - r_b->rank;
}

void add_to_queue(Request queue[], int* size, Request req) {
    queue[*size] = req;
    (*size)++;
    qsort(queue, *size, sizeof(Request), compare_requests);
}

void remove_from_queue_by_rank(Request queue[], int* size, int rank_to_remove) {
    int i, j;
    for (i = 0; i < *size; i++) {
        if (queue[i].rank == rank_to_remove) {
            for (j = i; j < (*size) - 1; j++) {
                queue[j] = queue[j + 1];
            }
            (*size)--;
            return;
        }
    }
}

int find_my_request_index(Request queue[], int size, int my_rank) {
    for (int i = 0; i < size; i++) {
        if (queue[i].rank == my_rank) {
            return i;
        }
    }
    return -1;
}

int max(int a, int b) {
    return a > b ? a : b;
}

const char* get_message_type_name(MessageType type) {
    switch (type) {
        case MSG_STEAL_REQ: return "żądanie KRADZIEŻY (STEAL_REQ)";
        case MSG_STEAL_REL: return "zwolnienie KRADZIEŻY (STEAL_REL)";
        case MSG_TERMINATE: return "ZAKOŃCZENIE PRACY (TERMINATE)";
        default: return "NIEZNANY TYP";
    }
}

int main(int argc, char* argv[]) {
    int my_rank, num_procs;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);

    int clock = 0;

    // Zmieniono: tablice dla każdego domu
    Request steal_requests_queues[NUM_HOUSES_TOTAL][num_procs];
    int steal_requests_queue_sizes[NUM_HOUSES_TOTAL];
    int highest_ts_received_steal[NUM_HOUSES_TOTAL][num_procs];
    bool is_active[num_procs];

    // Inicjalizacja struktur dla każdego domu
    for (int house = 0; house < NUM_HOUSES_TOTAL; house++) {
        steal_requests_queue_sizes[house] = 0;
        for (int i = 0; i < num_procs; i++) {
            highest_ts_received_steal[house][i] = 0;
        }
    }
    for (int i = 0; i < num_procs; i++) {
        is_active[i] = true;
    }

    srand(my_rank * time(NULL));

    for (int op_count = 0; op_count < NUM_OPERATIONS; op_count++) {
        int target_house_id = (my_rank + op_count) % NUM_HOUSES_TOTAL;
        printf("--- Proces %d --- [Zegar: %d] Rozpoczynam Operację #%d: Próba kradzieży w domu %d. Zwiększam zegar.\n", 
               my_rank, clock, op_count + 1, target_house_id);

        clock++;
        clock++;
        Request my_steal_req = {clock, my_rank};
        add_to_queue(steal_requests_queues[target_house_id], 
                    &steal_requests_queue_sizes[target_house_id], 
                    my_steal_req);
        
        Message msg_out_steal = {MSG_STEAL_REQ, clock, my_rank, target_house_id};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_out_steal, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("--- Proces %d --- [Zegar: %d] Wysłałem **%s** dla domu %d z moim czasem (ts=%d) do wszystkich innych procesów.\n", 
               my_rank, clock, get_message_type_name(MSG_STEAL_REQ), target_house_id, clock);

        while (true) {
            int my_idx_steal = find_my_request_index(
                steal_requests_queues[target_house_id], 
                steal_requests_queue_sizes[target_house_id], 
                my_rank
            );
            bool can_enter_steal_cs = (my_idx_steal == 0 && steal_requests_queue_sizes[target_house_id] > 0);
            
            if (can_enter_steal_cs) {
                for (int i = 0; i < num_procs; i++) {
                    if (i == my_rank || !is_active[i]) continue;
                    bool received_later_message_from_i =
                        (highest_ts_received_steal[target_house_id][i] > my_steal_req.timestamp) ||
                        (highest_ts_received_steal[target_house_id][i] == my_steal_req.timestamp && i > my_rank);
                    if (!received_later_message_from_i) {
                        can_enter_steal_cs = false;
                        printf("--- Proces %d --- [Zegar: %d] Muszę czekać na odpowiedź od procesu %d dla domu %d. Nie mogę wejść do sekcji kradzieży.\n", 
                               my_rank, clock, i, target_house_id);
                        break;
                    }
                }
            }

            if (can_enter_steal_cs) {
                break;
            }

            int flag;
            MPI_Status status;
            MPI_Iprobe(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &flag, &status);
            if (flag) {
                Message msg_in;
                MPI_Recv(&msg_in, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
                clock = max(clock, msg_in.timestamp) + 1;
                printf("--- Proces %d --- [Zegar: %d] Odebrałem wiadomość: **%s** od procesu %d dla domu %d z timestampem (ts=%d). Aktualizuję zegar.\n", 
                       my_rank, clock, get_message_type_name(msg_in.type), msg_in.sender_rank, msg_in.house_id, msg_in.timestamp);

                if (msg_in.type == MSG_TERMINATE) {
                    is_active[msg_in.sender_rank] = false;
                    printf("--- Proces %d --- [Zegar: %d] Oznaczam proces %d jako nieaktywny.\n", 
                           my_rank, clock, msg_in.sender_rank);
                } else {
                    int house = msg_in.house_id;
                    highest_ts_received_steal[house][msg_in.sender_rank] = 
                        max(highest_ts_received_steal[house][msg_in.sender_rank], msg_in.timestamp);

                    if (msg_in.type == MSG_STEAL_REQ) {
                        Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                        add_to_queue(steal_requests_queues[house], 
                                    &steal_requests_queue_sizes[house], 
                                    new_req);
                        printf("--- Proces %d --- [Zegar: %d] Dodano żądanie do kolejki dla domu %d od procesu %d.\n", 
                               my_rank, clock, house, msg_in.sender_rank);
                    } else if (msg_in.type == MSG_STEAL_REL) {
                        remove_from_queue_by_rank(steal_requests_queues[house], 
                                                &steal_requests_queue_sizes[house], 
                                                msg_in.sender_rank);
                        printf("--- Proces %d --- [Zegar: %d] Usunięto żądanie z kolejki dla domu %d od procesu %d.\n", 
                               my_rank, clock, house, msg_in.sender_rank);
                    }
                }
            } else {
                usleep(1000);
            }
        }

        clock++;
        printf("--- Proces %d --- [Zegar: %d] *** WSZEDŁEM DO SEKCJI KRYTYCZNEJ KRADZIEŻY! *** Okradam dom %d.\n", 
               my_rank, clock, target_house_id);
        usleep((rand() % 100 + 50) * 1000);

        clock++;
        remove_from_queue_by_rank(steal_requests_queues[target_house_id], 
                                 &steal_requests_queue_sizes[target_house_id], 
                                 my_rank);
        Message msg_steal_rel = {MSG_STEAL_REL, clock, my_rank, target_house_id};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_steal_rel, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("--- Proces %d --- [Zegar: %d] *** WYSZEDŁEM Z SEKCJI KRYTYCZNEJ KRADZIEŻY. *** Wysłałem wiadomość **%s** dla domu %d (ts=%d) do wszystkich innych procesów.\n", 
               my_rank, clock, get_message_type_name(MSG_STEAL_REL), target_house_id, clock);
        printf("--- Proces %d --- [Zegar: %d] Zakończyłem Operacj #%d (dom %d). Odpoczywam przed kolejną próbą.\n", 
               my_rank, clock, op_count + 1, target_house_id);
        usleep((rand() % 50) * 1000);
    }

    Message msg_terminate = {MSG_TERMINATE, clock, my_rank, -1};
    for (int i = 0; i < num_procs; i++) {
        if (i != my_rank) {
            MPI_Send(&msg_terminate, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
        }
    }
    printf("--- Proces %d --- [Zegar: %d] Wysłałem wiadomość **%s** do wszystkich innych procesów przed zakończeniem.\n", 
           my_rank, clock, get_message_type_name(MSG_TERMINATE));

    printf("--- Proces %d --- [Zegar: %d] Zakończyłem wszystkie zaplanowane operacje kradzieży. Finalizuję pracę.\n", 
           my_rank, clock);
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
    return 0;
}