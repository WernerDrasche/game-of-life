#include <SFML/Graphics.hpp>
#include <iostream>
#include <bitset>
#include <fstream>
#include <string>
#include <vector>
#include <random>
#include "nfd.h"

#define CELL_SIZE_INIT 25
#define CELL_SIZE_MAX 100
#define GRID_THICKNESS 2
#define PAN_PPF 5
#define SCROLL_PPF 1
#define FRAMES_PER_TICK_INIT 60
#define DRAW_GRID_THRESHOLD 8

enum class FileDialogMode { Open, Save };

std::string file_dialog(FileDialogMode mode) {
    nfdchar_t *path = nullptr;
    nfdresult_t result;
    if (mode == FileDialogMode::Open) {
        result = NFD_OpenDialog("gol", nullptr, &path);
    } else {
        result = NFD_SaveDialog("gol", nullptr, &path);
    }
    if (result == NFD_ERROR) {
        std::cerr << "Error: " << NFD_GetError() << std::endl;
    }
    return result == NFD_OKAY ? path : "";
}

uint32_t delta_swap(uint32_t a, uint32_t mask, uint8_t shift) {
    uint32_t b = ((a << shift) ^ a) & mask;
    a ^= b ^ (b >> shift);
    return a;
}

uint32_t interleaveXY(uint16_t x, uint16_t y) {
    uint32_t res = (y << 16) | x;
    res = delta_swap(res, 0b00000000111111110000000000000000, 8);
    res = delta_swap(res, 0b00001111000000000000111100000000, 4);
    res = delta_swap(res, 0b00110000001100000011000000110000, 2);
    res = delta_swap(res, 0b01000100010001000100010001000100, 1);
    return res;
}

enum Cell : uint8_t {
    Dead = 0,
    Alive = 1,
    Dying = 3,
    Birthing = 2,
    DeadVisited = 4,
};

// Z-curve
class GameField {
    static const uint32_t MASK_X = 0b01010101010101010101010101010101;
    static const uint32_t MASK_Y = 0b10101010101010101010101010101010;

    uint8_t n;
    size_t size_x;
    size_t size_y;
public:
    Cell *cells;
    size_t size;
    size_t idx;

    GameField(size_t size) : size(size), idx(0) {
        const uint8_t bits = sizeof(size) * 8;
        uint8_t n;
        for (n = 0; n < bits; (size >>= 1, ++n)) {
            if (size & 1) {
                size >>= 1;
                break;
            }
        }
        if (n == bits || size) {
            throw std::invalid_argument("size needs to be power of 2");
        }
        if (n > 12) {
            throw std::invalid_argument("size >4096 not supported");
        }
        this->n = n;
        size_t len = 1 << (n << 1);
        cells = new Cell[len];
        memset(cells, Dead, len);
        size_x = interleaveXY(this->size, 0);
        size_y = size_x << 1;
    }

    ~GameField() {
        delete[] cells;
    }

    GameField &operator=(GameField &&other) {
        this->~GameField();
        std::memmove(this, &other, sizeof(GameField));
        other.cells = nullptr;
        return *this;
    }

    void clear() {
        size_t n = size * size;
        for (size_t i = 0; i < n; ++i) {
            cells[i] = Dead;
        }
    }

    Cell setCursor(size_t x, size_t y) {
        if (x >= size || y >= size) {
            return DeadVisited;
        }
        idx = interleaveXY(x, y);
        return cells[idx];
    }

    void toggle() {
        cells[idx] = cells[idx] == Dead ? Alive : Dead;
    }

    void setAlive(uint32_t *idxs, size_t len) {
        size_t max_idx = size * size;
        for (size_t i = 0; i < len; ++i) {
            size_t idx = idxs[i];
            if (idx < max_idx) {
                cells[idx] = Alive;
            }
        }
    }

    Cell right(uint8_t mask = Alive) {
        uint32_t y = idx & MASK_Y;
        uint32_t x = ((idx | MASK_Y) + 1) & MASK_X;
        idx = y | x;
        if (x >= size_x || y >= size_y) {
            return (Cell)(DeadVisited & mask);
        }
        return (Cell)(cells[idx] & mask);
    }

    Cell left(uint8_t mask = Alive) {
        uint32_t y = idx & MASK_Y;
        uint32_t x = ((idx & MASK_X) - 1) & MASK_X;
        idx = y | x;
        if (x >= size_x || y >= size_y) {
            return (Cell)(DeadVisited & mask);
        }
        return (Cell)(cells[idx] & mask);
    }

    Cell up(uint8_t mask = Alive) {
        uint32_t x = idx & MASK_X;
        uint32_t y = ((idx & MASK_Y) - 1) & MASK_Y;
        idx = x | y;
        if (y >= size_y || x >= size_x) {
            return (Cell)(DeadVisited & mask);
        }
        return (Cell)(cells[idx] & mask);
    }

    Cell down(uint8_t mask = Alive) {
        uint32_t x = idx & MASK_X;
        uint32_t y = ((idx | MASK_X) + 1) & MASK_Y;
        idx = x | y;
        if (y >= size_y || x >= size_x) {
            return (Cell)(DeadVisited & mask);
        }
        return (Cell)(cells[idx] & mask);
    }

    unsigned char countAliveNeighbors() {
        unsigned char count = 0;
        size_t old_idx = idx;
        count += up();
        count += left();
        count += down();
        count += down();
        count += right();
        count += right();
        count += up();
        count += up();
        idx = old_idx;
        return count;
    }

    void updateCell() {
        unsigned char alive_neighbors = countAliveNeighbors();
        if (cells[idx] == Alive && (alive_neighbors < 2 || alive_neighbors > 3)) {
            cells[idx] = Dying;
        } else if (cells[idx] == Dead) {
            cells[idx] = alive_neighbors == 3 ? Birthing : DeadVisited;
        }
    }

    void populateRandom() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint8_t> dist(0, 1);
        size_t len = size * size;
        for (size_t i = 0; i < len; ++i) {
            cells[i] = (Cell)dist(gen);
        }
    }
};

class Game {
    sf::RectangleShape shape_alive;
    static const unsigned int grid_thickness_threshold =
        (CELL_SIZE_INIT + DRAW_GRID_THRESHOLD) / 2;
    unsigned int grid_thickness;
public:
    GameField field;
    sf::Window &window;
    unsigned int cell_size;
    int origin_x, origin_y;

    Game(size_t size, sf::Window &window) : 
        field(size),
        window(window),
        cell_size(CELL_SIZE_INIT),
        grid_thickness(GRID_THICKNESS)
    {
        shape_alive = sf::RectangleShape(sf::Vector2f(CELL_SIZE_INIT, CELL_SIZE_INIT));
        shape_alive.setFillColor(sf::Color::Black);
        sf::Vector2u window_size = window.getSize();
        center();
    }

    void center() {
        sf::Vector2u window_size = window.getSize();
        size_t field_center = (field.size * cell_size) / 2;
        origin_x = field_center - window_size.x / 2;
        origin_y = field_center - window_size.y / 2;
    }

    void openFile(std::string path) {
        std::ifstream file(path, std::ios::binary | std::ios::in | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "Error: unable to open file " << path << std::endl;
            return;
        }
        std::streampos size = file.tellg();
        if (size < 4 && size % 4 != 0) {
            std::cerr << "Error: wrong file format";
        }
        size_t len = size / 4;
        uint32_t *idxs = new uint32_t[len];
        file.seekg(0, std::ios::beg);
        file.read((char *)idxs, size);
        file.close();
        size_t new_size = idxs[0];
        if (new_size != field.size) {
            try {
                field = std::move(GameField(new_size));
            } catch (const std::invalid_argument &e) {
                std::cerr << e.what() << std::endl;
                return;
            } 
        }
        field.clear();
        field.setAlive(idxs + 1, len - 1);
        center();
    }

    void saveFile(std::string path) {
        if (!path.ends_with(".gol")) {
            path += ".gol";
        }
        std::ofstream file(path, std::ios::binary | std::ios::out);
        if (!file.is_open()) {
            std::cerr << "Error: unable to open file " << path << std::endl;
            return;
        }
        std::vector<uint32_t> vec{};
        vec.push_back(field.size);
        uint32_t len = field.size * field.size;
        for (uint32_t i = 0; i < len; ++i) {
            if (field.cells[i] == Alive) {
                vec.push_back(i);
            }
        }
        file.write((char *)vec.data(), vec.size() * 4);
        file.close();
    }

    void updateCellSize(int delta) {
        cell_size = std::clamp((int)cell_size + delta, 1, CELL_SIZE_MAX);
        shape_alive.setSize(sf::Vector2f(cell_size, cell_size));
        if (cell_size < grid_thickness_threshold) {
            grid_thickness = GRID_THICKNESS / 2;
        } else {
            grid_thickness = GRID_THICKNESS;
        }
    }

    void simulationTick() {
        const uint8_t mask = Alive | DeadVisited;
        size_t idx = 0;
        for (size_t y = 0; y < field.size; ++y) {
            for (size_t x = 0; x < field.size; ++x) {
                Cell cell = field.cells[idx];
                if (cell == Alive) {
                    field.idx = idx;
                    field.updateCell();
                    if (field.up(mask) == Dead)
                        field.updateCell();
                    if (field.left(mask) == Dead)
                        field.updateCell();
                    if (field.down(mask) == Dead)
                        field.updateCell();
                    if (field.down(mask) == Dead)
                        field.updateCell();
                    if (field.right(mask) == Dead)
                        field.updateCell();
                    if (field.right(mask) == Dead)
                        field.updateCell();
                    if (field.up(mask) == Dead)
                        field.updateCell();
                    if (field.up(mask) == Dead)
                        field.updateCell();
                }
                ++idx;
            }
        }
        idx = 0;
        for (size_t y = 0; y < field.size; ++y) {
            for (size_t x = 0; x < field.size; ++x) {
                Cell cell = field.cells[idx];
                if (cell == Birthing) {
                    field.cells[idx] = Alive;
                } else if (cell == Dying || cell == DeadVisited) {
                    field.cells[idx] = Dead;
                }
                ++idx;
            }
        }
    }

    void draw(sf::RenderWindow &window) {
        sf::Vector2u window_size = window.getSize();
        int pixel_x_start, pixel_y_start, pixel_x, pixel_y;
        size_t coord_x_start, coord_y_start, coord_x, coord_y;
        if (origin_x <= 0) {
            pixel_x_start = -origin_x;
            coord_x_start = 0;
        } else {
            pixel_x_start = -(origin_x % cell_size);
            coord_x_start = std::min(origin_x / cell_size, (unsigned int)field.size);
        }
        if (origin_y <= 0) {
            pixel_y_start = -origin_y;
            coord_y_start = 0;
        } else {
            pixel_y_start = -(origin_y % cell_size);
            coord_y_start = std::min(origin_y / cell_size, (unsigned int)field.size);
        }
        Cell cell = field.setCursor(coord_x_start, coord_y_start);
        for ((pixel_y = pixel_y_start, coord_y = coord_y_start);
                pixel_y < (int)window_size.y && coord_y < field.size;
                (pixel_y += cell_size, ++coord_y))
        {
            size_t idx = field.idx;
            for ((pixel_x = pixel_x_start, coord_x = coord_x_start);
                    pixel_x < (int)window_size.x && coord_x < field.size;
                    (pixel_x += cell_size, ++coord_x))
            {
                if (cell == Alive) {
                    shape_alive.setPosition(pixel_x, pixel_y);
                    window.draw(shape_alive);
                }
                cell = field.right();
            }
            field.idx = idx;
            cell = field.down();
        }
        unsigned int horiz_len = pixel_x - pixel_x_start;
        unsigned int vert_len = pixel_y - pixel_y_start;
        if (cell_size >= DRAW_GRID_THRESHOLD) {
            sf::RectangleShape shape_line_horiz(sf::Vector2f(horiz_len, grid_thickness));
            shape_line_horiz.setFillColor(sf::Color::Black);
            for ((pixel_y = pixel_y_start, coord_y = coord_y_start);
                    pixel_y < (int)window_size.y && coord_y <= field.size;
                    (pixel_y += cell_size, ++coord_y))
            {
                shape_line_horiz.setPosition(pixel_x_start, pixel_y);
                window.draw(shape_line_horiz);
            }
            sf::RectangleShape shape_line_vert(sf::Vector2f(grid_thickness, vert_len));
            shape_line_vert.setFillColor(sf::Color::Black);
            for ((pixel_x = pixel_x_start, coord_x = coord_x_start);
                    pixel_x < (int)window_size.x && coord_x <= field.size;
                    (pixel_x += cell_size, ++coord_x))
            {
                shape_line_vert.setPosition(pixel_x, pixel_y_start);
                window.draw(shape_line_vert);
            }
        }
    }
};

int main() {
    sf::RenderWindow window(sf::VideoMode(512, 512), "SFML");
    window.setVerticalSyncEnabled(true);
    window.setFramerateLimit(60);
    window.setKeyRepeatEnabled(false);
    Game game(2048, window);
    unsigned int old_mouse_x, old_mouse_y;
    bool panning_mode = false;
    bool simulating = false;
    unsigned int frames_till_next_tick = 0;
    unsigned int frames_per_tick = FRAMES_PER_TICK_INIT;
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            switch (event.type) {
                case sf::Event::Closed:
                    window.close();
                    break;
                case sf::Event::KeyPressed:
                    if (event.key.code == sf::Keyboard::Space) {
                        simulating = !simulating;
                        frames_till_next_tick = 0;
                    }
                    if (event.key.code == sf::Keyboard::C) {
                        game.field.clear();
                    } else if (event.key.code == sf::Keyboard::R) {
                        game.field.populateRandom();
                    }
                    if (event.key.code == sf::Keyboard::O) {
                        std::string path = file_dialog(FileDialogMode::Open);
                        if (!path.empty()) {
                            game.openFile(path);
                        }
                    } else if (event.key.code == sf::Keyboard::S) {
                        std::string path = file_dialog(FileDialogMode::Save);
                        if (!path.empty()) {
                            game.saveFile(path);
                        }
                    }
                    if (event.key.code == sf::Keyboard::Up) {
                        if (frames_per_tick > 10) {
                            frames_per_tick -= 10;
                        } else if (frames_per_tick > 5) {
                            frames_per_tick -= 5;
                        } else if (frames_per_tick > 1) {
                            frames_per_tick -= 2;
                        }
                    } else if (event.key.code == sf::Keyboard::Down) {
                        if (frames_per_tick < 5) {
                            frames_per_tick += 2;
                        } else if (frames_per_tick < 10) {
                            frames_per_tick += 5;
                        } else if (frames_per_tick < 60) {
                            frames_per_tick += 10;
                        }
                    }
                    break;
                case sf::Event::MouseWheelScrolled:
                    if (event.mouseWheelScroll.wheel == sf::Mouse::VerticalWheel) {
                        int pixel_x = event.mouseWheelScroll.x + game.origin_x;
                        int pixel_y = event.mouseWheelScroll.y + game.origin_y;
                        double coord_x_old = (double)pixel_x / game.cell_size;
                        double coord_y_old = (double)pixel_y / game.cell_size;
                        if (event.mouseWheelScroll.delta < 0) {
                            game.updateCellSize(-SCROLL_PPF);
                        } else {
                            game.updateCellSize(SCROLL_PPF);
                        }
                        double coord_x = (double)pixel_x / game.cell_size;
                        double coord_y = (double)pixel_y / game.cell_size;
                        game.origin_x -= (coord_x - coord_x_old) * game.cell_size;
                        game.origin_y -= (coord_y - coord_y_old) * game.cell_size;
                    }
                    break;
                case sf::Event::MouseButtonPressed:
                    if (event.mouseButton.button == sf::Mouse::Right) {
                        panning_mode = true;
                        old_mouse_x = event.mouseButton.x;
                        old_mouse_y = event.mouseButton.y;
                    }
                    if (event.mouseButton.button == sf::Mouse::Left) {
                        int pixel_x = event.mouseButton.x + game.origin_x;
                        int pixel_y = event.mouseButton.y + game.origin_y;
                        if (pixel_x >= 0 && pixel_y >= 0) {
                            size_t coord_x = pixel_x / game.cell_size;
                            size_t coord_y = pixel_y / game.cell_size;
                            if (coord_x < game.field.size && coord_y < game.field.size) {
                                game.field.setCursor(coord_x, coord_y);
                                game.field.toggle();
                            }
                        }
                    }
                    break;
                case sf::Event::MouseButtonReleased:
                    panning_mode = false;
                    break;
                default:
                    break;
            }
        }
        if (window.hasFocus()) {
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::H)) {
                game.origin_x += PAN_PPF;
            } else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::L)) {
                game.origin_x -= PAN_PPF;
            }
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::K)) {
                game.origin_y += PAN_PPF;
            } else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::J)) {
                game.origin_y -= PAN_PPF;
            }
            if (panning_mode && sf::Mouse::isButtonPressed(sf::Mouse::Right)) {
                sf::Vector2i new_pos = sf::Mouse::getPosition(window);
                game.origin_x -= new_pos.x - old_mouse_x;
                game.origin_y -= new_pos.y - old_mouse_y;
                old_mouse_x = new_pos.x;
                old_mouse_y = new_pos.y;
            }
        }
        if (simulating && frames_till_next_tick-- == 0) {
            game.simulationTick();
            frames_till_next_tick = frames_per_tick;
        }
        window.clear(sf::Color::White);
        game.draw(window);
        window.display();
    }
}
