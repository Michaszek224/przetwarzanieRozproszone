#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For usleep
#include <time.h>   // For time()
#include <stdbool.h> // For bool, true, false

#define NUM_HOUSES_TOTAL 10 // Przykładowa łączna liczba domów (zasobów)
#define P_FENCES 4          // Liczba dostępnych paserów
#define MAX_PROCS 16        // Maksymalna liczba procesów
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
            // Nie sortujemy ponownie po usunięciu, bo Lamport tego nie wymaga
            // qsort(queue, *size, sizeof(Request), compare_requests);
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

int main(int argc, char* argv[]) {
    int my_rank, num_procs;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);

    if (num_procs > MAX_PROCS) {
        if (my_rank == 0) printf("Zbyt wiele procesów, zmień MAX_PROCS.\n");
        MPI_Finalize();
        return 1;
    }

    int clock = 0;
    Request steal_requests_queue[MAX_PROCS];
    int steal_requests_queue_size = 0;
    Request fence_requests_queue[MAX_PROCS];
    int fence_requests_queue_size = 0;

    // Tablice do śledzenia najwyższego timestampu otrzymanego od każdego procesu
    int highest_ts_received_steal[MAX_PROCS];
    int highest_ts_received_fence[MAX_PROCS];
    for(int i=0; i<num_procs; ++i) {
        highest_ts_received_steal[i] = 0;
        highest_ts_received_fence[i] = 0;
    }


    srand(my_rank * time(NULL)); // Różne ziarna dla różnych procesów

    for (int op_count = 0; op_count < NUM_OPERATIONS; op_count++) {
        // --- SEKCJA KRADZIEŻY ---
        printf("[%d] Zegar: %d, Operacja #%d: Rozpoczynam próbę kradzieży.\n", my_rank, clock, op_count + 1);

        clock++;
        int target_house_id = (my_rank + op_count) % NUM_HOUSES_TOTAL;
        printf("[%d] Zegar: %d, Celuję w dom (symboliczny ID: %d).\n", my_rank, clock, target_house_id);

        clock++;
        Request my_steal_req = {clock, my_rank};
        add_to_queue(steal_requests_queue, &steal_requests_queue_size, my_steal_req);
        printf("[%d] Zegar: %d, Dodałem własne żądanie kradzieży (ts=%d) do kolejki. Kolejka (%d): ", my_rank, clock, my_steal_req.timestamp, steal_requests_queue_size);
        for(int i=0; i<steal_requests_queue_size; ++i) printf("(%d,%d) ", steal_requests_queue[i].timestamp, steal_requests_queue[i].rank);
        printf("\n");


        Message msg_out_steal = {MSG_STEAL_REQ, my_steal_req.timestamp, my_rank};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_out_steal, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("[%d] Zegar: %d, Wysłałem STEAL_REQ (ts=%d) do wszystkich.\n", my_rank, clock, my_steal_req.timestamp);


        while (true) { 
            int my_idx_steal = find_my_request_index(steal_requests_queue, steal_requests_queue_size, my_rank);
            
            bool can_enter_steal_cs = (my_idx_steal == 0 && steal_requests_queue_size > 0);
            if (can_enter_steal_cs) { // Jeśli moje żądanie jest na czele kolejki
                for (int i = 0; i < num_procs; i++) { // Sprawdź dla każdego innego procesu i
                    if (i == my_rank) continue;

                    // Warunek Lamporta: muszę otrzymać od procesu 'i' wiadomość (ts_msg, rank_msg_sender=i)
                    // taką, że (ts_msg, i) > (moje_żądanie_ts, mój_rank)
                    // highest_ts_received_steal[i] to ts_msg ostatniej wiadomości od procesu i
                    bool received_later_message_from_i =
                        (highest_ts_received_steal[i] > my_steal_req.timestamp) ||
                        (highest_ts_received_steal[i] == my_steal_req.timestamp && i > my_rank);

                    if (!received_later_message_from_i) {
                        can_enter_steal_cs = false; // Nie spełniono warunku dla procesu i, muszę czekać
                        break;
                    }
                }
            }

            if (can_enter_steal_cs) {
                break; // Mogę wejść do sekcji krytycznej
            }

            Message msg_in;
            MPI_Status status;
            MPI_Recv(&msg_in, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
            
            clock = max(clock, msg_in.timestamp) + 1;
            printf("[%d] Zegar: %d, Odebrałem wiadomość typu %d od %d (ts=%d).\n", my_rank, clock, msg_in.type, msg_in.sender_rank, msg_in.timestamp);

            if (msg_in.type == MSG_STEAL_REQ || msg_in.type == MSG_STEAL_REL) {
                 highest_ts_received_steal[msg_in.sender_rank] = max(highest_ts_received_steal[msg_in.sender_rank], msg_in.timestamp);
            } else if (msg_in.type == MSG_FENCE_REQ || msg_in.type == MSG_FENCE_REL) {
                 highest_ts_received_fence[msg_in.sender_rank] = max(highest_ts_received_fence[msg_in.sender_rank], msg_in.timestamp);
            }


            if (msg_in.type == MSG_STEAL_REQ) {
                Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                add_to_queue(steal_requests_queue, &steal_requests_queue_size, new_req);
                 printf("[%d] Zegar: %d, Dodałem STEAL_REQ od %d (ts=%d) do kolejki. Kolejka (%d): ", my_rank, clock, msg_in.sender_rank, msg_in.timestamp, steal_requests_queue_size);
                 for(int i=0; i<steal_requests_queue_size; ++i) printf("(%d,%d) ", steal_requests_queue[i].timestamp, steal_requests_queue[i].rank);
                 printf("\n");
            } else if (msg_in.type == MSG_STEAL_REL) {
                remove_from_queue_by_rank(steal_requests_queue, &steal_requests_queue_size, msg_in.sender_rank);
                 printf("[%d] Zegar: %d, Usunąłem STEAL_REQ od %d z kolejki po STEAL_REL. Kolejka (%d): ", my_rank, clock, msg_in.sender_rank, steal_requests_queue_size);
                 for(int i=0; i<steal_requests_queue_size; ++i) printf("(%d,%d) ", steal_requests_queue[i].timestamp, steal_requests_queue[i].rank);
                 printf("\n");
            } else if (msg_in.type == MSG_FENCE_REQ) { // Obsługa "niespodziewanych" wiadomości paserskich
                Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                add_to_queue(fence_requests_queue, &fence_requests_queue_size, new_req);
            } else if (msg_in.type == MSG_FENCE_REL) {
                remove_from_queue_by_rank(fence_requests_queue, &fence_requests_queue_size, msg_in.sender_rank);
            }
            // Upewnij się, że odpowiednia kolejka jest posortowana (add_to_queue już to robi)
            // qsort(steal_requests_queue, steal_requests_queue_size, sizeof(Request), compare_requests);
        }

        clock++;
        printf("[%d] Zegar: %d, WSZEDŁEM DO SEKCJI KRYTYCZNEJ KRADZIEŻY (dom %d).\n", my_rank, clock, target_house_id);

        printf("[%d] Zegar: %d, Okradam dom %d...\n", my_rank, clock, target_house_id);
        usleep((rand() % 100 + 50) * 10000); //strzałka

        clock++;
        remove_from_queue_by_rank(steal_requests_queue, &steal_requests_queue_size, my_rank);
        
        Message msg_steal_rel = {MSG_STEAL_REL, clock, my_rank};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_steal_rel, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("[%d] Zegar: %d, WYSZEDŁEM Z SEKCJI KRYTYCZNEJ KRADZIEŻY. Wysłałem STEAL_REL (ts=%d).\n", my_rank, clock, clock);

        // --- SEKCJA PASERA ---
        printf("[%d] Zegar: %d, Rozpoczynam próbę zajęcia pasera.\n", my_rank, clock);

        clock++;
        Request my_fence_req = {clock, my_rank};
        add_to_queue(fence_requests_queue, &fence_requests_queue_size, my_fence_req);
        printf("[%d] Zegar: %d, Dodałem własne żądanie pasera (ts=%d) do kolejki. Kolejka (%d): ", my_rank, clock, my_fence_req.timestamp, fence_requests_queue_size);
        for(int i=0; i<fence_requests_queue_size; ++i) printf("(%d,%d) ", fence_requests_queue[i].timestamp, fence_requests_queue[i].rank);
        printf("\n");

        Message msg_out_fence = {MSG_FENCE_REQ, my_fence_req.timestamp, my_rank};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_out_fence, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("[%d] Zegar: %d, Wysłałem FENCE_REQ (ts=%d) do wszystkich.\n", my_rank, clock, my_fence_req.timestamp);

        while (true) { 
            int my_idx_fence = find_my_request_index(fence_requests_queue, fence_requests_queue_size, my_rank);
            
            bool can_enter_fence_cs = (my_idx_fence != -1 && my_idx_fence < P_FENCES && fence_requests_queue_size > 0);

            if (can_enter_fence_cs) { // Jeśli kwalifikuję się do pasera na podstawie pozycji w kolejce
                 for (int i = 0; i < num_procs; i++) { // Sprawdź warunek Lamporta dla każdego innego procesu i
                    if (i == my_rank) continue;
                    
                    bool received_later_message_from_i =
                        (highest_ts_received_fence[i] > my_fence_req.timestamp) ||
                        (highest_ts_received_fence[i] == my_fence_req.timestamp && i > my_rank);

                    if (!received_later_message_from_i) {
                        can_enter_fence_cs = false; // Nie spełniono warunku, muszę czekać
                        break;
                    }
                }
            }
            
            if (can_enter_fence_cs) {
                break; // Mogę wejść do sekcji krytycznej pasera
            }

            Message msg_in;
            MPI_Status status;
            MPI_Recv(&msg_in, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

            clock = max(clock, msg_in.timestamp) + 1;
            printf("[%d] Zegar: %d, Odebrałem wiadomość typu %d od %d (ts=%d).\n", my_rank, clock, msg_in.type, msg_in.sender_rank, msg_in.timestamp);
            
            if (msg_in.type == MSG_STEAL_REQ || msg_in.type == MSG_STEAL_REL) {
                 highest_ts_received_steal[msg_in.sender_rank] = max(highest_ts_received_steal[msg_in.sender_rank], msg_in.timestamp);
            } else if (msg_in.type == MSG_FENCE_REQ || msg_in.type == MSG_FENCE_REL) {
                 highest_ts_received_fence[msg_in.sender_rank] = max(highest_ts_received_fence[msg_in.sender_rank], msg_in.timestamp);
            }


            if (msg_in.type == MSG_FENCE_REQ) {
                Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                add_to_queue(fence_requests_queue, &fence_requests_queue_size, new_req);
                printf("[%d] Zegar: %d, Dodałem FENCE_REQ od %d (ts=%d) do kolejki. Kolejka (%d): ", my_rank, clock, msg_in.sender_rank, msg_in.timestamp, fence_requests_queue_size);
                for(int i=0; i<fence_requests_queue_size; ++i) printf("(%d,%d) ", fence_requests_queue[i].timestamp, fence_requests_queue[i].rank);
                printf("\n");
            } else if (msg_in.type == MSG_FENCE_REL) {
                remove_from_queue_by_rank(fence_requests_queue, &fence_requests_queue_size, msg_in.sender_rank);
                printf("[%d] Zegar: %d, Usunąłem FENCE_REQ od %d z kolejki po FENCE_REL. Kolejka (%d): ", my_rank, clock, msg_in.sender_rank, fence_requests_queue_size);
                for(int i=0; i<fence_requests_queue_size; ++i) printf("(%d,%d) ", fence_requests_queue[i].timestamp, fence_requests_queue[i].rank);
                printf("\n");
            } else if (msg_in.type == MSG_STEAL_REQ) { // Obsługa "niespodziewanych" wiadomości o kradzież
                Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                add_to_queue(steal_requests_queue, &steal_requests_queue_size, new_req);
            } else if (msg_in.type == MSG_STEAL_REL) {
                remove_from_queue_by_rank(steal_requests_queue, &steal_requests_queue_size, msg_in.sender_rank);
            }
            // qsort(fence_requests_queue, fence_requests_queue_size, sizeof(Request), compare_requests);
        }

        clock++;
        printf("[%d] Zegar: %d, WSZEDŁEM DO SEKCJI KRYTYCZNEJ PASERA (moja pozycja w kolejce: %d).\n", my_rank, clock, find_my_request_index(fence_requests_queue, fence_requests_queue_size, my_rank));

        printf("[%d] Zegar: %d, Pracuję z paserem...\n", my_rank, clock);
        usleep((rand() % 80 + 30) * 10000); //strzałka

        clock++;
        remove_from_queue_by_rank(fence_requests_queue, &fence_requests_queue_size, my_rank);
        
        Message msg_fence_rel = {MSG_FENCE_REL, clock, my_rank};
        for (int i = 0; i < num_procs; i++) {
            if (i != my_rank) {
                MPI_Send(&msg_fence_rel, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("[%d] Zegar: %d, WYSZEDŁEM Z SEKCJI KRYTYCZNEJ PASERA. Wysłałem FENCE_REL (ts=%d).\n", my_rank, clock, clock);

        printf("[%d] Zegar: %d, Zakończyłem operację #%d.\n", my_rank, clock, op_count + 1);
        usleep((rand() % 50) * 10000); //strzałka
    }

    printf("[%d] Zegar: %d, Zakończyłem wszystkie operacje. Finalizuję.\n", my_rank, clock);
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
    return 0;
}
