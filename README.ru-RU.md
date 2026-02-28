[![en](https://img.shields.io/badge/lang-en-green.svg)](README.md)

Стрим-сервер
-

**Модуль** для **Apostol CRM**[^crm].

Описание
-
**StreamServer** — модуль приёма UDP-датаграмм, работающий в том же процессе Апостол, что и HTTP-сервер. Принимает бинарные или текстовые датаграммы на настраиваемом UDP-порту и передаёт каждую датаграмму в функцию PostgreSQL для обработки.

Модуль состоит из двух уровней:

- **StreamServer** (базовый класс) — создаёт неблокирующий UDP-сокет, регистрирует его в цикле событий на основе `epoll` и вызывает виртуальный callback `on_datagram()` для каждого входящего пакета.
- **PgStreamServer** (подкласс) — переопределяет `on_datagram()`, передавая каждую датаграмму в функцию PostgreSQL `stream.parse(protocol, data)` асинхронно через пул соединений.

Основные характеристики:

- Написан на C++20 с использованием асинхронной неблокирующей модели ввода-вывода на базе **epoll** API.
- UDP-сокет и HTTP-слушатель разделяют один цикл событий — без потоков, без дополнительных процессов.
- Базовый класс `StreamServer` может быть расширен для обработки датаграмм без PostgreSQL.

Принцип работы
-
1. При вызове `on_start()` модуль создаёт `UdpSocket`, привязанный к настроенному порту, и регистрирует его в EventLoop на события `EPOLLIN`.
2. При получении датаграммы цикл событий вызывает `on_datagram(data, size, sender_addr)`.
3. `PgStreamServer` выполняет асинхронный вызов `SELECT stream.parse('<protocol>', '<hex_data>')`.
4. При вызове `on_stop()` сокет снимается с регистрации и закрывается.

Настройка
-

```json
{
  "modules": {
    "StreamServer": {
      "enabled": true,
      "port": 12228
    }
  }
}
```

| Параметр | Описание |
|----------|----------|
| `enabled` | Включение/отключение модуля. |
| `port` | UDP-порт для прослушивания (0 = отключён). |

Установка
-
Следуйте указаниям по сборке и установке [Апостол (C++20)](https://github.com/apostoldevel/libapostol#build-and-installation).

[^crm]: **Apostol CRM** — шаблон-проект построенный на фреймворках [A-POST-OL](https://github.com/apostoldevel/libapostol) (C++20) и [PostgreSQL Framework for Backend Development](https://github.com/apostoldevel/db-platform).
