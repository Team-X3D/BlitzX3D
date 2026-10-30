#ifndef TILE_H
#define TILE_H

enum { EAX = 1, ECX, EDX, EDI, ESI, EBX };

enum {
	REG_MASK_ALL = (1 << EAX) | (1 << ECX) | (1 << EDX) | (1 << EDI) | (1 << ESI) | (1 << EBX),
	REG_MASK_BYTE = (1 << EAX) | (1 << ECX) | (1 << EDX) | (1 << EBX)
};

struct Tile {

	int want_l, want_r, hits, argFrame;
	int allowed_l, allowed_r;
	bool forceOrder;

	Tile(const std::string& a, Tile* l = 0, Tile* r = 0);
	Tile(const std::string& a, const std::string& a2, Tile* l = 0, Tile* r = 0);
	~Tile();

	void label();
	int  eval(int want);

private:
	int  need;
	Tile* l, * r;
	std::string assem, assem2;

};

#endif