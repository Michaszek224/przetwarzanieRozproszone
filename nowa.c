#include <mpi.h>         // MPI biblioteka do komunikacji międzyprocesowej
#include <stdio.h>       // Standardowe wejście/wyjście (np. printf)
#include <stdlib.h>      // Standardowe funkcje biblioteczne (np. rand, qsort, malloc)
#include <string.h>      // Funkcje do operacji na stringach (nieużywane bezpośrednio, ale często przydatne)
#include <unistd.h>      // Dla funkcji usleep (pauza)
#include <time.h>        // Dla funkcji time() (inicjalizacja generatora liczb losowych)
#include <stdbool.h>     // Dla typów bool, true, false

#define NUM_HOUSES_TOTAL 2 // Całkowita liczba domów
#define NUM_OPERATIONS 2   // Ile razy każdy proces (złodziej) spróbuje coś ukraść

// Typy wiadomości używane w komunikacji MPI
typedef enum {
    MSG_REQ,         // Żądanie dostępu do sekcji krytycznej (Request)
    MSG_ACK,         // Potwierdzenie / Zgoda (Acknowledgement)
    MSG_TERMINATE    // Wiadomość informująca o zakończeniu pracy przez proces
} MessageType;

// Struktura wiadomości przesyłanej między procesami
typedef struct {
    MessageType type;      // Typ wiadomości (z enum MessageType)
    int timestamp;         // Zegar Lamporta nadawcy wiadomości
    int sender_rank;       // Ranga (ID) procesu wysyłającego wiadomość
} Message;

// Prosta funkcja zwracająca większą z dwóch liczb.
int max(int a, int b) {
    return a > b ? a : b;
}

// Funkcja pomocnicza do zwracania nazwy typu wiadomości jako string (dla logowania).
const char* get_message_type_name(MessageType type) {
    switch (type) {
        case MSG_REQ: return "ŻĄDANIE (REQ)";
        case MSG_ACK: return "POTWIERDZENIE (ACK)";
        case MSG_TERMINATE: return "ZAKOŃCZENIE PRACY (TERMINATE)";
        default: return "NIEZNANY TYP";
    }
}

int main(int argc, char* argv[]) {
    int my_rank, num_procs; // Ranga bieżącego procesu i całkowita liczba procesów
    MPI_Init(&argc, &argv); // Inicjalizacja środowiska MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);    // Pobranie rangi bieżącego procesu
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);  // Pobranie całkowitej liczby procesów

    int clock = 0; // Zegar Lamporta dla bieżącego procesu
    
    // Statusy procesu Ricarta-Agrawali
    bool requesting_cs = false; // Czy proces aktualnie ubiega się o sekcję krytyczną
    int my_request_timestamp = -1; // Timestamp mojego bieżącego żądania
    int replies_received_count = 0; // Licznik otrzymanych ACK

    // Kolejka żądań do opóźnionych ACK (dla procesów, które muszą czekać na moje zwolnienie)
    int deferred_reply_queue[num_procs];
    int deferred_reply_queue_size = 0;

    // Tablice do śledzenia stanu aktywności innych procesów (dla terminacji)
    bool is_active[num_procs];
    for(int i=0; i<num_procs; ++i) {
        is_active[i] = true;
    }

    srand(my_rank * time(NULL)); // Inicjalizacja generatora liczb losowych

    // Główna pętla symulująca operacje kradzieży
    for (int op_count = 0; op_count < NUM_OPERATIONS; op_count++) {
        // --- PRÓBA WEJŚCIA DO SEKCJI KRYTYCZNEJ ---
        printf("--- Proces %d --- [Zegar: %d] Rozpoczynam Operację #%d: Próba kradzieży. Zwiększam zegar.\n", my_rank, clock, op_count + 1);
        
        clock++; // Zdarzenie lokalne: inkrementacja zegara Lamporta
        requesting_cs = true; // Ustawiam flagę, że ubiegam się o SC
        my_request_timestamp = clock; // Zapamiętuję timestamp mojego żądania
        replies_received_count = 0; // Resetuję licznik ACK

        Message msg_out_req = {MSG_REQ, my_request_timestamp, my_rank}; // Przygotowanie wiadomości REQ
        // printf("--- Proces %d --- [Zegar: %d] Wysyłam **%s** (ts=%d) do wszystkich.\n", my_rank, clock, get_message_type_name(MSG_REQ), my_request_timestamp);
        for (int i = 0; i < num_procs; i++) { // Rozesłanie żądania do wszystkich innych procesów
            if (i != my_rank) {
                MPI_Send(&msg_out_req, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }

        // Pętla oczekiwania na możliwość wejścia do sekcji krytycznej (Ricart-Agrawala)
        while (replies_received_count < (num_procs - 1)) { // Czekaj na ACK od wszystkich N-1 procesów
            int flag; // Flaga wskazująca, czy jest dostępna wiadomość
            MPI_Status status; // Status operacji MPI_Recv
            
            // Sprawdzenie, czy są wiadomości (nieblokujące)
            MPI_Iprobe(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &flag, &status);

            if (flag) { // Jeśli jest wiadomość
                Message msg_in;
                MPI_Recv(&msg_in, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status); // Odbierz wiadomość
                
                // Aktualizacja zegara Lamporta na podstawie odebranej wiadomości
                clock = max(clock, msg_in.timestamp) + 1;
                // printf("--- Proces %d --- [Zegar: %d] Odebrałem wiadomość: **%s** od procesu %d (ts=%d).\n", my_rank, clock, get_message_type_name(msg_in.type), msg_in.sender_rank, msg_in.timestamp);

                // Przetwarzanie wiadomości w zależności od jej typu
                if (msg_in.type == MSG_REQ) { // Żądanie dostępu od innego procesu
                    bool defer_reply = false; // Czy opóźnić odpowiedź?

                    // Jeśli ja się ubiegam ORAZ moje żądanie ma wyższy priorytet (niższy timestamp)
                    // LUB jeśli moje żądanie ma ten sam timestamp, ale moją rangę jest niższa (reguła rozstrzygania remisu)
                    if (requesting_cs && 
                        ((my_request_timestamp < msg_in.timestamp) || 
                         (my_request_timestamp == msg_in.timestamp && my_rank < msg_in.sender_rank))) {
                        defer_reply = true; // Opóźnij odpowiedź, bo mam wyższy priorytet
                    }

                    if (defer_reply) {
                        // Dodaj nadawcę do kolejki oczekujących na ACK
                        deferred_reply_queue[deferred_reply_queue_size++] = msg_in.sender_rank;
                        // printf("--- Proces %d --- [Zegar: %d] Opóźniam ACK dla procesu %d. Moje żądanie (ts=%d) ma wyższy priorytet.\n", my_rank, clock, msg_in.sender_rank, my_request_timestamp);
                    } else {
                        // Wysyłam ACK od razu
                        clock++; // Zdarzenie lokalne: wysłanie ACK
                        Message msg_out_ack = {MSG_ACK, clock, my_rank};
                        MPI_Send(&msg_out_ack, sizeof(Message), MPI_BYTE, msg_in.sender_rank, 0, MPI_COMM_WORLD);
                        // printf("--- Proces %d --- [Zegar: %d] Wysłałem **%s** do procesu %d.\n", my_rank, clock, get_message_type_name(MSG_ACK), msg_in.sender_rank);
                    }
                } else if (msg_in.type == MSG_ACK) { // Otrzymanie ACK od innego procesu
                    replies_received_count++; // Zwiększ licznik otrzymanych ACK
                    // printf("--- Proces %d --- [Zegar: %d] Otrzymałem ACK od procesu %d. Liczba ACK: %d/%d\n", my_rank, clock, msg_in.sender_rank, replies_received_count, num_procs - 1);
                } else if (msg_in.type == MSG_TERMINATE) { // Wiadomość o zakończeniu pracy od innego procesu
                    is_active[msg_in.sender_rank] = false; // Oznacz proces jako nieaktywny
                    // Jeśli proces zakończył pracę, a ja go brałem pod uwagę do ACK, to mogę to uznać za otrzymane ACK
                    // Jest to uproszczenie, aby symulacja nie zawieszała się na oczekiwaniu na nieaktywne procesy.
                    // W bardziej robustnym systemie należałoby to rozwiązać inaczej (np. algorytm kworum).
                    replies_received_count++; 
                    printf("--- Proces %d --- [Zegar: %d] Proces %d zakończył pracę. Uznaję to jako otrzymane ACK.\n", my_rank, clock, msg_in.sender_rank);
                }
            } else {
                usleep(10000); // Krótka pauza, aby nie obciążać CPU w pętli oczekiwania
            }
        }

        // --- WEJŚCIE DO SEKCJI KRYTYCZNEJ ---
        clock++; // Zdarzenie lokalne: inkrementacja zegara
        int target_house_id = (my_rank + op_count) % NUM_HOUSES_TOTAL; // Symboliczny wybór domu do okradzenia
        printf("--- Proces %d --- [Zegar: %d] *** WSZEDŁEM DO SEKCJI KRYTYCZNEJ KRADZIEŻY! *** Okradam dom o symbolicznym ID: %d.\n", my_rank, clock, target_house_id);

        usleep((rand() % 100 + 50) * 1000); // Symulacja czasu trwania kradzieży

        // --- WYJŚCIE Z SEKCJI KRYTYCZNEJ ---
        clock++; // Zdarzenie lokalne: inkrementacja zegara
        requesting_cs = false; // Nie ubiegam się już o SC

        printf("--- Proces %d --- [Zegar: %d] *** WYSZEDŁEM Z SEKCJI KRYTYCZNEJ KRADZIEŻY. *** Wysyłam opóźnione ACK.\n", my_rank, clock);
        // Wysyłanie opóźnionych ACK
        for (int i = 0; i < deferred_reply_queue_size; i++) {
            int target_rank = deferred_reply_queue[i];
            Message msg_out_ack = {MSG_ACK, clock, my_rank}; // ACK z aktualnym timestampem
            MPI_Send(&msg_out_ack, sizeof(Message), MPI_BYTE, target_rank, 0, MPI_COMM_WORLD);
            // printf("--- Proces %d --- [Zegar: %d] Wysłałem opóźnione **%s** do procesu %d.\n", my_rank, clock, get_message_type_name(MSG_ACK), target_rank);
        }
        deferred_reply_queue_size = 0; // Wyczyść kolejkę opóźnionych odpowiedzi

        printf("--- Proces %d --- [Zegar: %d] Zakończyłem Operację #%d. Odpoczywam przed kolejną próbą.\n", my_rank, clock, op_count + 1);
        usleep((rand() % 50) * 1000); // Symulacja odpoczynku
    }

    // Po zakończeniu wszystkich operacji, proces informuje inne procesy o swoim zakończeniu
    Message msg_terminate = {MSG_TERMINATE, clock, my_rank}; // Przygotuj wiadomość o zakończeniu
    for (int i = 0; i < num_procs; i++) {
        if (i != my_rank) {
            MPI_Send(&msg_terminate, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD); // Wyślij do innych
        }
    }
    printf("--- Proces %d --- [Zegar: %d] Zakończyłem wszystkie zaplanowane operacje kradzieży. Finalizuję pracę.\n", my_rank, clock);

    MPI_Barrier(MPI_COMM_WORLD); // Bariera, aby upewnić się, że wszystkie procesy doszły do tego punktu przed finalizacją
    MPI_Finalize(); // Zakończenie pracy środowiska MPI
    return 0;
}