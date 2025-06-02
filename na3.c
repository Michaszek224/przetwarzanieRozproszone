#include <mpi.h>      // MPI biblioteka do komunikacji międzyprocesowej
#include <stdio.h>    // Standardowe wejście/wyjście (np. printf)
#include <stdlib.h>   // Standardowe funkcje biblioteczne (np. rand, qsort, malloc)
#include <string.h>   // Funkcje do operacji na stringach (nieużywane bezpośrednio, ale często przydatne)
#include <unistd.h>   // Dla funkcji usleep (pauza)
#include <time.h>     // Dla funkcji time() (inicjalizacja generatora liczb losowych)
#include <stdbool.h>  // Dla typów bool, true, false

#define NUM_HOUSES_TOTAL 5 // Całkowita liczba domów do okradzenia (symboliczna)
#define NUM_OPERATIONS 2   // Ile razy każdy proces (złodziej) spróbuje coś ukraść

// Typy wiadomości używane w komunikacji MPI
typedef enum {
    MSG_STEAL_REQ,    // Żądanie wejścia do sekcji krytycznej "kradzież"
    MSG_STEAL_REL,    // Zwolnienie sekcji krytycznej "kradzież"
    MSG_TERMINATE     // Wiadomość informująca o zakończeniu pracy przez proces
} MessageType;

// Struktura wiadomości przesyłanej między procesami
typedef struct {
    MessageType type;    // Typ wiadomości (z enum MessageType)
    int timestamp;       // Zegar Lamporta nadawcy wiadomości
    int sender_rank;     // Ranga (ID) procesu wysyłającego wiadomość
} Message;

// Struktura reprezentująca żądanie w kolejce (dla algorytmu Lamporta)
typedef struct {
    int timestamp;       // Zegar Lamporta żądania
    int rank;            // Ranga (ID) procesu, który wysłał żądanie
} Request;

// Funkcja pomocnicza do sortowania żądań w kolejce
// Sortuje najpierw wg timestampu, a w przypadku remisów wg rangi procesu.
int compare_requests(const void* a, const void* b) {
    Request* r_a = (Request*)a;
    Request* r_b = (Request*)b;
    if (r_a->timestamp != r_b->timestamp) {
        return r_a->timestamp - r_b->timestamp; // Rosnąco wg timestampu
    }
    return r_a->rank - r_b->rank; // Rosnąco wg rangi
}

// Dodaje żądanie do kolejki i utrzymuje ją posortowaną.
void add_to_queue(Request queue[], int* size, Request req) {
    queue[*size] = req; // Dodaj na koniec
    (*size)++;          // Zwiększ rozmiar kolejki
    qsort(queue, *size, sizeof(Request), compare_requests); // Posortuj kolejkę
}

// Usuwa żądanie z kolejki na podstawie rangi procesu.
void remove_from_queue_by_rank(Request queue[], int* size, int rank_to_remove) {
    int i, j;
    for (i = 0; i < *size; i++) {
        if (queue[i].rank == rank_to_remove) { // Znaleziono żądanie do usunięcia
            for (j = i; j < (*size) - 1; j++) {
                queue[j] = queue[j + 1]; // Przesuń elementy w lewo
            }
            (*size)--; // Zmniejsz rozmiar kolejki
            return;
        }
    }
}

// Znajduje indeks własnego żądania w kolejce.
int find_my_request_index(Request queue[], int size, int my_rank) {
    for (int i = 0; i < size; i++) {
        if (queue[i].rank == my_rank) {
            return i; // Zwraca indeks, jeśli znaleziono
        }
    }
    return -1; // Zwraca -1, jeśli nie znaleziono
}

// Prosta funkcja zwracająca większą z dwóch liczb.
int max(int a, int b) {
    return a > b ? a : b;
}

// Funkcja pomocnicza do zwracania nazwy typu wiadomości jako string (dla logowania).
const char* get_message_type_name(MessageType type) {
    switch (type) {
        case MSG_STEAL_REQ: return "żądanie KRADZIEŻY (STEAL_REQ)";
        case MSG_STEAL_REL: return "zwolnienie KRADZIEŻY (STEAL_REL)";
        case MSG_TERMINATE: return "ZAKOŃCZENIE PRACY (TERMINATE)";
        default: return "NIEZNANY TYP";
    }
}

int main(int argc, char* argv[]) {
    int my_rank, num_procs; // Ranga bieżącego procesu i całkowita liczba procesów
    MPI_Init(&argc, &argv); // Inicjalizacja środowiska MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);   // Pobranie rangi bieżącego procesu
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs); // Pobranie całkowitej liczby procesów

    int clock = 0; // Zegar Lamporta dla bieżącego procesu
    Request steal_requests_queue[num_procs]; // Kolejka żądań dostępu do sekcji krytycznej "kradzież"
    int steal_requests_queue_size = 0;       // Aktualny rozmiar kolejki żądań kradzieży

    // Tablice do śledzenia stanu komunikacji z innymi procesami dla algorytmu Lamporta
    int highest_ts_received_steal[num_procs]; // Najwyższy timestamp odebrany od procesu i dla żądań kradzieży
    bool is_active[num_procs];                // Flagi śledzące, które procesy są nadal aktywne

    // Inicjalizacja tablic śledzących
    for(int i=0; i<num_procs; ++i) {
        highest_ts_received_steal[i] = 0;
        is_active[i] = true; // Na początku wszystkie procesy są uznawane za aktywne
    }

    srand(my_rank * time(NULL)); // Inicjalizacja generatora liczb losowych (różne ziarno dla każdego procesu)

    // Główna pętla symulująca operacje kradzieży
    for (int op_count = 0; op_count < NUM_OPERATIONS; op_count++) {
        // --- SEKCJA KRADZIEŻY ---
        printf("--- Proces %d --- [Zegar: %d] Rozpoczynam Operację #%d: Próba kradzieży. Zwiększam zegar.\n", my_rank, clock, op_count + 1);

        clock++; // Zdarzenie lokalne: inkrementacja zegara Lamporta
        int target_house_id = (my_rank + op_count) % NUM_HOUSES_TOTAL; // Symboliczny wybór domu do okradzenia

        // Przygotowanie i wysłanie żądania wejścia do sekcji krytycznej "kradzież"
        clock++; // Zdarzenie lokalne: inkrementacja zegara Lamporta przed wysłaniem żądania
        Request my_steal_req = {clock, my_rank}; // Utworzenie własnego żądania
        add_to_queue(steal_requests_queue, &steal_requests_queue_size, my_steal_req); // Dodanie żądania do lokalnej kolejki
        
        Message msg_out_steal = {MSG_STEAL_REQ, my_steal_req.timestamp, my_rank}; // Przygotowanie wiadomości
        for (int i = 0; i < num_procs; i++) { // Rozesłanie żądania do wszystkich innych procesów
            if (i != my_rank) {
                MPI_Send(&msg_out_steal, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        // printf("--- Proces %d --- [Zegar: %d] Wysłałem **%s** z moim czasem (ts=%d) do wszystkich innych procesów.\n", my_rank, clock, get_message_type_name(MSG_STEAL_REQ), my_steal_req.timestamp);

        // Pętla oczekiwania na możliwość wejścia do sekcji krytycznej (algorytm Lamporta)
        while (true) { 
            int my_idx_steal = find_my_request_index(steal_requests_queue, steal_requests_queue_size, my_rank); // Znajdź moje żądanie w kolejce
            
            // Warunek 1 Lamporta: Moje żądanie jest na czele posortowanej kolejki
            bool can_enter_steal_cs = (my_idx_steal == 0 && steal_requests_queue_size > 0);
            
            if (can_enter_steal_cs) { 
                // Warunek 2 Lamporta: Otrzymałem wiadomość od każdego innego aktywnego procesu
                // z timestampem późniejszym niż moje żądanie (lub tym samym timestampem i wyższą rangą).
                // printf("--- Proces %d --- [Zegar: %d] Sprawdzam, czy moje żądanie kradzieży (ts=%d, rank=%d) jest na czele kolejki.\n", my_rank, clock, my_steal_req.timestamp, my_rank);
                for (int i = 0; i < num_procs; i++) { 
                    if (i == my_rank || !is_active[i]) continue; // Pomiń siebie i nieaktywne procesy

                    // Sprawdzenie, czy otrzymano wiadomość od procesu 'i' z późniejszym timestampem
                    bool received_later_message_from_i =
                        (highest_ts_received_steal[i] > my_steal_req.timestamp) ||
                        (highest_ts_received_steal[i] == my_steal_req.timestamp && i > my_rank); // Rozstrzyganie remisów rangą

                    if (!received_later_message_from_i) {
                        can_enter_steal_cs = false; // Jeśli nie, nie mogę jeszcze wejść do sekcji krytycznej
                        // printf("--- Proces %d --- [Zegar: %d] Muszę czekać na odpowiedź od procesu %d lub na późniejsze żądanie. Nie mogę wejść do sekcji kradzieży.\n", my_rank, clock, i);
                        break; // Przerwij sprawdzanie, jeden proces blokuje
                    }
                }
            }

            if (can_enter_steal_cs) {
                break; // Mogę wejść do sekcji krytycznej, wyjdź z pętli oczekiwania
            }

            // Odbieranie i przetwarzanie wiadomości w pętli oczekiwania
            int flag; // Flaga wskazująca, czy jest dostępna wiadomość
            MPI_Status status; // Status operacji MPI_Recv
            MPI_Iprobe(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &flag, &status); // Nieblokujące sprawdzenie, czy są wiadomości

            if (flag) { // Jeśli jest wiadomość
                Message msg_in;
                MPI_Recv(&msg_in, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status); // Odbierz wiadomość
                
                // Aktualizacja zegara Lamporta na podstawie odebranej wiadomości
                clock = max(clock, msg_in.timestamp) + 1;
                // printf("--- Proces %d --- [Zegar: %d] Odebrałem wiadomość: **%s** od procesu %d z timestampem (ts=%d). Aktualizuję zegar.\n", my_rank, clock, get_message_type_name(msg_in.type), msg_in.sender_rank, msg_in.timestamp);

                // Aktualizacja najwyższego odebranego timestampu dla odpowiedniego typu wiadomości i nadawcy
                if (msg_in.type == MSG_STEAL_REQ || msg_in.type == MSG_STEAL_REL) {
                    highest_ts_received_steal[msg_in.sender_rank] = max(highest_ts_received_steal[msg_in.sender_rank], msg_in.timestamp);
                }

                // Przetwarzanie wiadomości w zależności od jej typu
                if (msg_in.type == MSG_STEAL_REQ) { // Żądanie kradzieży od innego procesu
                    Request new_req = {msg_in.timestamp, msg_in.sender_rank};
                    add_to_queue(steal_requests_queue, &steal_requests_queue_size, new_req); // Dodaj do kolejki kradzieży
                } else if (msg_in.type == MSG_STEAL_REL) { // Zwolnienie sekcji kradzieży przez inny proces
                    remove_from_queue_by_rank(steal_requests_queue, &steal_requests_queue_size, msg_in.sender_rank); // Usuń z kolejki kradzieży
                } else if (msg_in.type == MSG_TERMINATE) { // Wiadomość o zakończeniu pracy od innego procesu
                    is_active[msg_in.sender_rank] = false; // Oznacz proces jako nieaktywny
                }
            } else {
                usleep(1000); // Krótka pauza, aby nie obciążać CPU w pętli oczekiwania
            }
        }

        // Wejście do sekcji krytycznej "kradzież"
        clock++; // Zdarzenie lokalne: inkrementacja zegara
        printf("--- Proces %d --- [Zegar: %d] *** WSZEDŁEM DO SEKCJI KRYTYCZNEJ KRADZIEŻY! *** Okradam dom o symbolicznym ID: %d.\n", my_rank, clock, target_house_id);

        usleep((rand() % 100 + 50) * 1000); // Symulacja czasu trwania kradzieży

        // Wyjście z sekcji krytycznej "kradzież"
        clock++; // Zdarzenie lokalne: inkrementacja zegara przed wysłaniem zwolnienia
        remove_from_queue_by_rank(steal_requests_queue, &steal_requests_queue_size, my_rank); // Usuń własne żądanie z kolejki
        
        Message msg_steal_rel = {MSG_STEAL_REL, clock, my_rank}; // Przygotuj wiadomość o zwolnieniu
        for (int i = 0; i < num_procs; i++) { // Rozgłoś wiadomość o zwolnieniu do wszystkich innych procesów
            if (i != my_rank) {
                MPI_Send(&msg_steal_rel, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
        printf("--- Proces %d --- [Zegar: %d] *** WYSZEDŁEM Z SEKCJI KRYTYCZNEJ KRADZIEŻY. *** Wysłałem wiadomość **%s** (ts=%d) do wszystkich innych procesów.\n", my_rank, clock, get_message_type_name(MSG_STEAL_REL), clock);
        printf("--- Proces %d --- [Zegar: %d] Zakończyłem Operacj #%d . Odpoczywam przed kolejną próbą.\n", my_rank, clock, op_count + 1);
        usleep((rand() % 50) * 1000); // Symulacja odpoczynku
    }

    // Po zakończeniu wszystkich operacji, proces informuje inne procesy o swoim zakończeniu
    Message msg_terminate = {MSG_TERMINATE, clock, my_rank}; // Przygotuj wiadomość o zakończeniu
    for (int i = 0; i < num_procs; i++) {
        if (i != my_rank) {
            MPI_Send(&msg_terminate, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD); // Wyślij do innych
        }
    }
    // printf("--- Proces %d --- [Zegar: %d] Wysłałem wiadomość **%s** do wszystkich innych procesów przed zakończeniem.\n", my_rank, clock, get_message_type_name(MSG_TERMINATE));

    printf("--- Proces %d --- [Zegar: %d] Zakończyłem wszystkie zaplanowane operacje kradzieży. Finalizuję pracę.\n", my_rank, clock);
    MPI_Barrier(MPI_COMM_WORLD); // Bariera, aby upewnić się, że wszystkie procesy doszły do tego punktu przed finalizacją
                                 // Pomaga to zapewnić, że wszystkie wiadomości TERMINATE zostaną wysłane i potencjalnie odebrane.
    MPI_Finalize(); // Zakończenie pracy środowiska MPI
    return 0;
}
