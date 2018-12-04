** ZSO Zadanie 2: Sterownik urządzenia HardDoom **
** Autor rozwiązania: Tom Macieszczak **

** Kompilacja **

Należy ustawić zmienną KDIR w pliku Makefile (bądź w środowisku) na katalog zawierający zbudowane źródła jądra w wersji 4.9.13
Następnie wywołać standardowe polecenie `make`.

** Krótki opis rozwiązania **

Stworzyłem standardowy moduł kernela, który rejestruje odpowiedni sterownik.
Kiedy pojawia się nowe urządzenie, tworzę dla niego dedykowaną strukturę `struct hd_dev` zawierającą wszystkie informacje potrzebne do jego obsługi (to zapewnia, że wszystkie zarejestrowane urządzenia działają niezależnie od siebie). Inicjuję urządzenie w`hd_probe`, gdzie alokuje dla niego zasoby, a także tworzę odpowiadające mu urządzenie znakowe. 

Zasoby tworzone przy pomocy zawołań `ioctl` są przechowywane w polu `private_data` odpowiedniego pliku.

Rozwiązanie działa w sposób synchroniczny i używa wbudowanej kolejki FIFO.
Wszelkie operacje rysowania wymagają posiadania mutexa właściwego dla danej instancji urządzenia. Dodatkowo, czytanie z ramki, uwolnienie pliku, oraz operacja `suspend` korzystają z polecenia PING_SYNC celem uzyskania pełnej synchronizacji. To gwarantuje, że dane zasoby nie są już używane przez urządzenie.

Oczekiwanie na wolne miejsce w kolejce zostało zaimplementowane zgodnie z proponowanym schematem używającym PING_ASYNC.

W celu zapewnienia, że urządzenie nie zostanie usunięte w czasie działania, użyłem zliczania referencji(kref). Utworzenie dowolnego zasobu zwiększa liczbę referencji na dane urządzenie. Urządzenie zostanie usunięte dopiero, gdy wszelkie zasoby z nim związane zostaną uwolnione.

Problem INTERLOCKów rozwiązałem przez zliczanie, ile poleceń INTERLOCK wystąpiło do danej chwili. Każda ramka bufora pamięta, ile INTERLOCKów wystąpiło do momentu, gdy ostatni raz do niej pisano. Nowe polecenie INTERLOCK jest potrzebne wtw. gdy globalna liczba INTERLOCKÓW dla danego urządzenia jest równa liczbie INTERLOCKów danej ramki. Oczywiście liczba interlocków może być ofiarą nadmiaru, wówczas zostanie wysłane nadmierne polecenie INTERLOCK. Biorąc jednak pod uwagę, że liczba jest 64-bitowa, a pamięć RAM ograniczona, co implikuje liczbę zasobów < 2^40, procent nadmiarowych INTERLOCKów będzie zaniedbywalny.
