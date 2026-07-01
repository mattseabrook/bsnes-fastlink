auto Program::filterSelect(uint& width, uint& height, uint scale) -> Filter::Render {
  Filter::None::size(width, height);
  return &Filter::None::render;
}
