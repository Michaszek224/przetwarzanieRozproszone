#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For usleep
#include <time.h>   // For time()
#include <stdbool.h> // For bool, true, false

#define NUM_HOUSES_TOTAL 1 // Przykładowa łączna liczba domów (zasobów)
#define P_FENCES 1          // Liczba dostępnych paserów
#define NUM_OPERATIONS 2    // Ile razy każdy złodziej spróbuje coś ukraść i spieniężyć

// Typy wiadomości
typedef enum {
    MSG_STEAL_REQ,
    MSG_STEAL_REL,
    MSG_FENCE_REQ,
    MSG_FENCE_REL
} MessageType;

// Struktura wiadomości
typedef struct {
    MessageType type;
    int timestamp;
    int sender_rank;
} Message;

// Struktura żądania w kolejce
typedef struct {
    int timestamp;
    int rank;
} Request;

// Funkcja pomocnicza do sortowania żądań (wg timestamp, potem wg rank)
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
    return -1; // Not found
}

int max(int a, int b) {
    return a > b ? a : b;
}

// Funkcja pomocnicza do zwracania nazwy typu wiadomości
const char* get_message_type_name(MessageType type) {
    switch (type) {
        case MSG_STEAL_REQ: return "żądanie KRADZIEŻY (STEAL_REQ)";
        case MSG_STEAL_REL: return "zwolnienie KRADZIEŻY (STEAL_REL)";
        case MSG_FENCE_REQ: return "żądanie PASERA (FENCE_REQ)";
        case MSG_FENCE_REL: return "zwolnienie PASERA (FENCE_REL)";
        default: return "NIEZNANY TYP";
    }
}

int main(int argc, char* argv[]) {
    int my_rank, num_procs;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);

    int clock = 0;
    Request steal_requests_queue[num_procs];
    int steal_requests_queue_size = 0;
    Request fence_requests_queue[num_procs];
    int fence_requests_queue_size = 0;

    int highest_ts_received_steal[num_procs];
    int highest_ts_received_fence[num_procs];
    for(int i=0; i<num_procs; ++i) {
        highest_ts_received_steal[i] = 0;
        highest_ts_received_fence[i] = 0;
    }

    srand(my_rank * time(NULL)); // Różne ziarna dla różnych procesów

    for (int op_count = 0; op_count < NUM_OPERATIONS; op_count++) {
        // --- SEKCJA KRADZIEŻY ---
        printf("--- Proces %d --- [Zegar: %d] Rozpoczynam Operację #%d: Próba kradzieży. Zwiększam zegar.\n", my_rank, clock, op_count + 1);

        clock++;
        int target_house_id = (my_rank + op_count) % NUM_HOUSES_TOTAL;

        clock++;
        Request my_steal_req = {clock, my_rank};
        add_to_queue(steal_requests_queue, &steal_requests_queue_size, my_steal_req);
        
        Message msg_out_steal = {MSG_STEAL_REQ, my_steal_req.timestamp, my_rank};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_out_steal, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("--- Proces %d --- [Zegar: %d] Wysłałem **%s** z moim czasem (ts=%d) do wszystkich innych procesów.\n", my_rank, clock, get_message_type_name(MSG_STEAL_REQ), my_steal_req.timestamp);

        while (true) { 
            int my_idx_steal = find_my_request_index(steal_requests_queue, steal_requests_queue_size, my_rank);
            
            bool can_enter_steal_cs = (my_idx_steal == 0 && steal_requests_queue_size > 0);
            if (can_enter_steal_cs) { 
                printf("--- Proces %d --- [Zegar: %d] Sprawdzam, czy moje żądanie kradzieży (ts=%d, rank=%d) jest na czele kolejki.\n", my_rank, clock, my_steal_req.timestamp, my_rank);
                for (int i = 0; i < num_procs; i++) { 
                    if (i == my_rank) continue;

                    bool received_later_message_from_i =
                        (highest_ts_received_steal[i] > my_steal_req.timestamp) ||
                        (highest_ts_received_steal[i] == my_steal_req.timestamp && i > my_rank);

                    if (!received_later_message_from_i) {
                        can_enter_steal_cs = false; 
                        printf("--- Proces %d --- [Zegar: %d] Muszę czekać na odpowiedź od procesu %d lub na późniejsze żądanie. Nie mogę wejść do sekcji kradzieży.\n", my_rank, clock, i);
                        break;
                    }
                }
            }

            if (can_enter_steal_cs) {
                break; 
            }

            Message msg_in;
            MPI_Status status;
            MPI_Recv(&msg_in, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
            
            clock = max(clock, msg_in.timestamp) + 1;
            printf("--- Proces %d --- [Zegar: %d] Odebrałem wiadomość: **%s** od procesu %d z timestampem (ts=%d). Aktualizuję zegar.\n", my_rank, clock, get_message_type_name(msg_in.type), msg_in.sender_rank, msg_in.timestamp);

            if (msg_in.type == MSG_STEAL_REQ || msg_in.type == MSG_STEAL_REL) {
                   highest_ts_received_steal[msg_in.sender_rank] = max(highest_ts_received_steal[msg_in.sender_rank], msg_in.timestamp);
            } else if (msg_in.type == MSG_FENCE_REQ || msg_in.type == MSG_FENCE_REL) {
                   highest_ts_received_fence[msg_in.sender_rank] = max(highest_ts_received_fence[msg_in.sender_rank], msg_in.timestamp);
            }

            if (msg_in.type == MSG_STEAL_REQ) {
                Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                add_to_queue(steal_requests_queue, &steal_requests_queue_size, new_req);
            } else if (msg_in.type == MSG_STEAL_REL) {
                remove_from_queue_by_rank(steal_requests_queue, &steal_requests_queue_size, msg_in.sender_rank);
            } else if (msg_in.type == MSG_FENCE_REQ) { 
                Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                add_to_queue(fence_requests_queue, &fence_requests_queue_size, new_req);
            } else if (msg_in.type == MSG_FENCE_REL) {
                remove_from_queue_by_rank(fence_requests_queue, &fence_requests_queue_size, msg_in.sender_rank);
            }
        }

        clock++;
        printf("--- Proces %d --- [Zegar: %d] *** WSZEDŁEM DO SEKCJI KRYTYCZNEJ KRADZIEŻY! *** Okradam dom o symbolicznym ID: %d.\n", my_rank, clock, target_house_id);

        usleep((rand() % 100 + 50) * 1000); 

        clock++;
        remove_from_queue_by_rank(steal_requests_queue, &steal_requests_queue_size, my_rank);
        
        Message msg_steal_rel = {MSG_STEAL_REL, clock, my_rank};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_steal_rel, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("--- Proces %d --- [Zegar: %d] *** WYSZEDŁEM Z SEKCJI KRYTYCZNEJ KRADZIEŻY. *** Wysłałem wiadomość **%s** (ts=%d) do wszystkich innych procesów.\n", my_rank, clock, get_message_type_name(MSG_STEAL_REL), clock);

        // --- SEKCJA PASERA ---
        printf("--- Proces %d --- [Zegar: %d] Rozpoczynam próbę zajęcia pasera (aby spieniężyć skradzione dobra).\n", my_rank, clock);

        clock++;
        Request my_fence_req = {clock, my_rank};
        add_to_queue(fence_requests_queue, &fence_requests_queue_size, my_fence_req);
        
        Message msg_out_fence = {MSG_FENCE_REQ, my_fence_req.timestamp, my_rank};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_out_fence, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("--- Proces %d --- [Zegar: %d] Wysłałem **%s** z moim czasem (ts=%d) do wszystkich innych procesów.\n", my_rank, clock, get_message_type_name(MSG_FENCE_REQ), my_fence_req.timestamp);

        while (true) { 
            int my_idx_fence = find_my_request_index(fence_requests_queue, fence_requests_queue_size, my_rank);
            
            bool can_enter_fence_cs = (my_idx_fence != -1 && my_idx_fence < P_FENCES && fence_requests_queue_size > 0);

            if (can_enter_fence_cs) { 
                printf("--- Proces %d --- [Zegar: %d] Sprawdzam, czy moje żądanie pasera jest wśród pierwszych %d w kolejce.\n", my_rank, clock, P_FENCES);
                for (int i = 0; i < num_procs; i++) { 
                    if (i == my_rank) continue;
                    
                    bool received_later_message_from_i =
                        (highest_ts_received_fence[i] > my_fence_req.timestamp) ||
                        (highest_ts_received_fence[i] == my_fence_req.timestamp && i > my_rank);

                    if (!received_later_message_from_i) {
                        can_enter_fence_cs = false; 
                        printf("--- Proces %d --- [Zegar: %d] Muszę czekać na odpowiedź od procesu %d lub na późniejsze żądanie. Nie mogę wejść do sekcji pasera.\n", my_rank, clock, i);
                        break;
                    }
                }
            }
            
            if (can_enter_fence_cs) {
                break; 
            }

            Message msg_in;
            MPI_Status status;
            MPI_Recv(&msg_in, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

            clock = max(clock, msg_in.timestamp) + 1;
            printf("--- Proces %d --- [Zegar: %d] Odebrałem wiadomość: **%s** od procesu %d z timestampem (ts=%d). Aktualizuję zegar.\n", my_rank, clock, get_message_type_name(msg_in.type), msg_in.sender_rank, msg_in.timestamp);
            
            if (msg_in.type == MSG_STEAL_REQ || msg_in.type == MSG_STEAL_REL) {
                   highest_ts_received_steal[msg_in.sender_rank] = max(highest_ts_received_steal[msg_in.sender_rank], msg_in.timestamp);
            } else if (msg_in.type == MSG_FENCE_REQ || msg_in.type == MSG_FENCE_REL) {
                   highest_ts_received_fence[msg_in.sender_rank] = max(highest_ts_received_fence[msg_in.sender_rank], msg_in.timestamp);
            }

            if (msg_in.type == MSG_FENCE_REQ) {
                Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                add_to_queue(fence_requests_queue, &fence_requests_queue_size, new_req);
            } else if (msg_in.type == MSG_FENCE_REL) {
                remove_from_queue_by_rank(fence_requests_queue, &fence_requests_queue_size, msg_in.sender_rank);
            } else if (msg_in.type == MSG_STEAL_REQ) { 
                Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                add_to_queue(steal_requests_queue, &steal_requests_queue_size, new_req);
            } else if (msg_in.type == MSG_STEAL_REL) {
                remove_from_queue_by_rank(steal_requests_queue, &steal_requests_queue_size, msg_in.sender_rank);
            }
        }

        clock++;
        printf("--- Proces %d --- [Zegar: %d] *** WSZEDŁEM DO SEKCJI KRYTYCZNEJ PASERA! *** Spieniężam skradzione dobra. Moja pozycja w kolejce: %d.\n", my_rank, clock, find_my_request_index(fence_requests_queue, fence_requests_queue_size, my_rank));

        usleep((rand() % 80 + 30) * 1000); 

        clock++;
        remove_from_queue_by_rank(fence_requests_queue, &fence_requests_queue_size, my_rank);
        
        Message msg_fence_rel = {MSG_FENCE_REL, clock, my_rank};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_fence_rel, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("--- Proces %d --- [Zegar: %d] *** WYSZEDŁEM Z SEKCJI KRYTYCZNEJ PASERA. *** Wysłałem wiadomość **%s** (ts=%d) do wszystkich innych procesów.\n", my_rank, clock, get_message_type_name(MSG_FENCE_REL), clock);

        printf("--- Proces %d --- [Zegar: %d] Zakończyłem Operację #%d (kradzież i spieniężenie). Odpoczywam przed kolejną próbą.\n", my_rank, clock, op_count + 1);
        usleep((rand() % 50) * 1000); 
    }

    printf("--- Proces %d --- [Zegar: %d] Zakończyłem wszystkie zaplanowane operacje kradzieży i spieniężania. Finalizuję pracę.\n", my_rank, clock);
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
    return 0;
}